#include "cell.h"

#include "arch/aarch64/smp.h"
#include "exec/stack.h"
#include "kprintf.h"
#include "mm/pmm.h"
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
static uint64_t loadavg_scaled[3];
static uint64_t next_loadavg_tick;

enum {
  LOADAVG_FSHIFT = 11,
  LOADAVG_FSCALE = 1u << LOADAVG_FSHIFT,
  LOADAVG_INTERVAL_TICKS = 5 * 100,
  LOADAVG_EXP_1 = 1884,
  LOADAVG_EXP_5 = 2014,
  LOADAVG_EXP_15 = 2037,
};

static struct domain *current_domain(void) {
  return cell_current_domain_internal();
}

static uint64_t runnable_thread_count(void) {
  uint64_t count = 0;
  for (size_t i = 0; i < cell_thread_capacity(); ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain != NULL && thread->state == THREAD_RUNNABLE) { ++count; }
  }
  return count;
}

static uint64_t loadavg_step(uint64_t old, uint64_t active, uint64_t exp) {
  return (old * exp + active * LOADAVG_FSCALE * (LOADAVG_FSCALE - exp)) >> LOADAVG_FSHIFT;
}

static void update_loadavg(void) {
  if (next_loadavg_tick == 0) { next_loadavg_tick = LOADAVG_INTERVAL_TICKS; }
  while (scheduler_ticks >= next_loadavg_tick) {
    uint64_t active = runnable_thread_count();
    loadavg_scaled[0] = loadavg_step(loadavg_scaled[0], active, LOADAVG_EXP_1);
    loadavg_scaled[1] = loadavg_step(loadavg_scaled[1], active, LOADAVG_EXP_5);
    loadavg_scaled[2] = loadavg_step(loadavg_scaled[2], active, LOADAVG_EXP_15);
    next_loadavg_tick += LOADAVG_INTERVAL_TICKS;
  }
}

static size_t clamp_size(size_t value, size_t min, size_t max) {
  if (value < min) { return min; }
  if (value > max) { return max; }
  return value;
}

static void derive_table_sizes(size_t *domains, size_t *threads, size_t *mms, size_t *open_files) {
  uint64_t pages = pmm_total_pages();
  size_t domain_count = clamp_size((size_t)(pages / 32768), 64, MAX_DOMAINS);
  size_t thread_count = clamp_size(domain_count * 2, 128, MAX_THREADS);
  size_t mm_count = domain_count + MAX_SNAPSHOTS;
  if (mm_count > MAX_DOMAINS + MAX_SNAPSHOTS) { mm_count = MAX_DOMAINS + MAX_SNAPSHOTS; }
  size_t open_file_count = clamp_size(domain_count * 4, 512, MAX_OPEN_FILES);
  *domains = domain_count;
  *threads = thread_count;
  *mms = mm_count;
  *open_files = open_file_count;
}

static void halt_table_init_failure(const char *name) {
  kprintf("[spore] failed to allocate %s table\n", name);
  for (;;) {
    __asm__ volatile("wfe");
  }
}

void cell_system_init(uint64_t hhdm_offset) {
  vma_set_hhdm_offset(hhdm_offset);
  size_t domain_count = 0;
  size_t thread_count = 0;
  size_t mm_count = 0;
  size_t open_file_count = 0;
  derive_table_sizes(&domain_count, &thread_count, &mm_count, &open_file_count);
  if (!cell_mm_reset(mm_count, hhdm_offset)) { halt_table_init_failure("mm"); }
  if (!cell_domain_reset(domain_count, hhdm_offset)) { halt_table_init_failure("domain"); }
  if (!cell_thread_reset(thread_count, hhdm_offset)) { halt_table_init_failure("thread"); }
  cell_snapshot_reset();
  if (!cell_fd_table_reset(open_file_count, hhdm_offset)) { halt_table_init_failure("open-file"); }
  cell_pipe_reset();
  scheduler_ticks = 0;
  scheduler_idle_ticks = 0;
  loadavg_scaled[0] = 0;
  loadavg_scaled[1] = 0;
  loadavg_scaled[2] = 0;
  next_loadavg_tick = LOADAVG_INTERVAL_TICKS;
  cell_tty_reset();
  kprintf("[spore] process tables: domains=%u threads=%u mms=%u open-files=%u\n", (unsigned)domain_count,
          (unsigned)thread_count, (unsigned)mm_count, (unsigned)open_file_count);
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
    update_loadavg();
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

void cell_loadavg_scaled(uint64_t out[3]) {
  if (out == NULL) { return; }
  out[0] = loadavg_scaled[0];
  out[1] = loadavg_scaled[1];
  out[2] = loadavg_scaled[2];
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
  for (size_t i = 0; i < cell_domain_capacity(); ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used) { continue; }
    if (seen == index) { return domain->id; }
    ++seen;
  }
  return 0;
}
