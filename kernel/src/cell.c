#include "cell.h"

#include "arch/aarch64/smp.h"
#include "exec/stack.h"
#include "kprintf.h"
#include "net.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/snapshot.h"
#include "proc/socket.h"
#include "proc/thread.h"
#include "proc/tty.h"

#include <stddef.h>

static uint64_t scheduler_ticks;
static uint64_t scheduler_idle_ticks;
static uint64_t boot_epoch_sec;
static struct domain *current_domain(void) {
  return cell_current_domain_internal();
}

void cell_system_init(uint64_t hhdm_offset) {
  vma_set_hhdm_offset(hhdm_offset);
  cell_mm_reset();
  cell_domain_reset();
  cell_thread_reset();
  cell_snapshot_reset();
  cell_fd_table_reset();
  cell_pipe_reset();
  scheduler_ticks = 0;
  scheduler_idle_ticks = 0;
  cell_tty_reset();
  // v2 Phase A object model: domains own isolation/policy state, threads own
  // EL0 execution state. Kernel mutation is serialized by the big kernel lock;
  // user execution may run on multiple CPUs.
}

bool cell_create_init(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp) {
  struct domain *domain = cell_alloc_domain();
  if (domain == NULL) { return false; }
  struct thread *thread = cell_alloc_thread(domain);
  if (thread == NULL) {
    domain->used = false;
    return false;
  }
  domain->parent_id = 0;
  domain->pgrp_id = domain->id;
  domain->session_id = domain->id;
  (void)cell_tty_set_foreground_pgrp(domain->pgrp_id);
  static const char *init_argv[] = {"/sbin/init"};
  cell_set_domain_identity(domain, "/sbin/init", init_argv, 1);
  struct process_mm *mm = cell_mm_from_owned(as, vmas);
  if (mm == NULL || !cell_domain_set_mm(domain, mm)) {
    if (mm != NULL) { cell_mm_release(mm); }
    thread->state = THREAD_UNUSED;
    domain->used = false;
    return false;
  }
  if (!vma_insert(cell_domain_vmas(domain), USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP,
                  VMM_USER_READ | VMM_USER_WRITE, 0, VMA_STACK)) {
    thread->state = THREAD_UNUSED;
    cell_mm_release(domain->mm);
    domain->mm = NULL;
    domain->used = false;
    return false;
  }
  if (!cell_init_stdio(domain)) {
    thread->state = THREAD_UNUSED;
    cell_mm_release(domain->mm);
    domain->mm = NULL;
    domain->used = false;
    return false;
  }
  thread->state = THREAD_RUNNABLE;
  thread->tf.elr_el1 = entry;
  thread->tf.sp_el0 = sp;
  thread->tf.spsr_el1 = 0x340;
  cell_set_current_thread(thread);
  kprintf("[spore] booting... domain %d / thread %d\n", domain->id, thread->tid);
  return true;
}

void cell_set_boot_epoch(uint64_t epoch_sec) {
  boot_epoch_sec = epoch_sec;
}

uint64_t cell_realtime_seconds(void) {
  return boot_epoch_sec + scheduler_ticks / 100;
}

void cell_timer_tick(struct trap_frame *frame, bool from_lower_el) {
  uint32_t cpu = smp_current_cpu();
  if (from_lower_el) {
    smp_note_cpu_busy_tick(cpu);
  } else if (cell_scheduler_waiting_for_interrupt()) {
    smp_note_cpu_idle_tick(cpu);
  }
  if (cpu == 0) {
    ++scheduler_ticks;
    if (cell_scheduler_waiting_for_interrupt()) { ++scheduler_idle_ticks; }
    cell_wake_sleep_waiters(scheduler_ticks);
    net_poll();
    cell_socket_timer_tick(scheduler_ticks);
    cell_wake_poll_waiters_internal();
  }
  struct domain *domain = current_domain();
  if (domain == NULL) { return; }
  if (from_lower_el) { ++domain->cpu_ticks; }
  if (domain->budget.max_ticks != 0 && domain->budget.remaining_ticks != 0) {
    --domain->budget.remaining_ticks;
    if (domain->budget.remaining_ticks == 0) {
      kprintf("[spore] domain %d exceeded CPU budget -> killed\n", domain->id);
      if (from_lower_el) {
        cell_exit_group_current(137, frame);
      } else {
        domain->zombie = true;
        domain->exit_status = 137;
      }
      return;
    }
  }
  if (from_lower_el) { cell_schedule(frame); }
}

uint64_t cell_uptime_ticks(void) {
  return scheduler_ticks;
}

uint64_t cell_idle_ticks(void) {
  return scheduler_idle_ticks;
}

uint64_t cell_cpu_busy_ticks(uint32_t cpu) {
  return smp_cpu_busy_ticks(cpu);
}

uint64_t cell_cpu_idle_ticks(uint32_t cpu) {
  return smp_cpu_idle_ticks(cpu);
}

uint64_t cell_boot_epoch_seconds(void) {
  return boot_epoch_sec;
}

bool cell_proc_exists(int pid) {
  return cell_find_domain(pid) != NULL;
}

uint32_t cell_proc_uid(int pid) {
  struct domain *domain = cell_find_domain(pid);
  return domain == NULL ? 0 : domain->uid;
}

uint32_t cell_proc_gid(int pid) {
  struct domain *domain = cell_find_domain(pid);
  return domain == NULL ? 0 : domain->gid;
}

void cell_note_unsupported_syscall(uint64_t nr) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return; }
  ++domain->unsupported_syscalls;
  domain->last_unsupported_syscall = nr;
}

void cell_note_unsupported_ioctl(uint64_t request) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return; }
  ++domain->unsupported_ioctls;
  domain->last_unsupported_ioctl = request;
}

int cell_proc_pid_at(size_t index) {
  size_t seen = 0;
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used) { continue; }
    if (seen == index) { return domain->id; }
    ++seen;
  }
  return 0;
}
