#include "mm/table.h"
#include "proc/thread.h"

#include "arch/platform.h"
#include "arch/regs.h"
#include "arch/smp.h"
#include "accounting_internal.h"
#include "proc/accounting.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "proc/process.h"
#include "proc/signal.h"
#include "net/net.h"
#include "net/socket.h"

constexpr int EINVAL = 22;
constexpr int ETIMEDOUT = 110;

static struct thread *threads;
static size_t thread_capacity;
static size_t thread_slot_limit;

bool cell_thread_reset(size_t capacity) {
  if (capacity == 0 || capacity > MAX_THREADS) { return false; }
  if (threads == NULL || thread_capacity != capacity) {
    threads = table_alloc(capacity, sizeof(*threads));
    if (threads == NULL) { return false; }
    thread_capacity = capacity;
  } else {
    kmemset(threads, 0, thread_capacity * sizeof(*threads));
  }
  cell_wait_reset();
  thread_slot_limit = 0;
  return true;
}

size_t cell_thread_slot_limit(void) {
  return thread_slot_limit;
}

int cell_current_tid(void) {
  struct thread *thread = cell_current_thread_internal();
  return thread == NULL ? 0 : thread->tid;
}

struct domain *cell_current_domain_internal(void) {
  struct thread *current_thread = cell_current_thread_internal();
  return current_thread == NULL ? NULL : current_thread->domain;
}

struct thread *cell_current_thread_internal(void) {
  uint32_t cpu = smp_current_cpu();
  return smp_current_thread_slot(cpu);
}

void cell_set_current_thread(struct thread *thread) {
  uint32_t cpu = smp_current_cpu();
  smp_set_current_thread_slot(cpu, thread);
  if (thread != NULL) { thread->running_cpu = (int)cpu; }
}

bool cell_scheduler_waiting_for_interrupt(void) {
  return smp_scheduler_waiting(smp_current_cpu());
}

struct thread *cell_thread_slot(size_t index) {
  return index < thread_capacity ? &threads[index] : NULL;
}

struct thread *cell_alloc_thread(struct domain *domain, bool leader) {
  for (size_t i = 0; i < thread_capacity; ++i) {
    if (threads[i].state == THREAD_UNUSED) {
      kmemset(&threads[i], 0, sizeof(threads[i]));
      if (i >= thread_slot_limit) { thread_slot_limit = i + 1; }
      threads[i].tid = leader ? domain->id : cell_new_task_id();
      threads[i].start_ticks = cell_uptime_ticks();
      threads[i].running_cpu = -1;
      threads[i].domain = domain;
      const struct thread *parent = cell_current_thread_internal();
      threads[i].capabilities = parent ? parent->capabilities : cell_initial_caps();
      threads[i].no_new_privs = parent && parent->no_new_privs;
      threads[i].wait.target = -1;
      threads[i].sigaltstack_flags = 2;
      ++domain->refcount;
      return &threads[i];
    }
  }
  return NULL;
}

void cell_release_thread(struct thread *thread) {
  if (thread == NULL || thread->domain == NULL) { return; }
  struct domain *domain = thread->domain;
  if (domain->refcount > 0) { --domain->refcount; }
  cell_clear_wait(thread);
  thread->state = THREAD_UNUSED;
  thread->running_cpu = -1;
  thread->domain = NULL;
  for (uint32_t cpu = 0; cpu < smp_present_cpu_count(); ++cpu) {
    smp_clear_current_thread_if(cpu, thread);
  }
}

struct thread *cell_find_thread(int tid) {
  for (size_t i = 0; i < thread_slot_limit; ++i) {
    if (threads[i].state != THREAD_UNUSED && threads[i].tid == tid) { return &threads[i]; }
  }
  return NULL;
}

struct thread *cell_thread_for_domain(const struct domain *domain) {
  for (size_t i = 0; i < thread_slot_limit; ++i) {
    if (threads[i].state != THREAD_UNUSED && threads[i].domain == domain) { return &threads[i]; }
  }
  return NULL;
}

size_t cell_runnable_or_blocked_threads_in_domain(const struct domain *domain) {
  size_t count = 0;
  for (size_t i = 0; i < thread_slot_limit; ++i) {
    if (threads[i].domain == domain && (threads[i].state == THREAD_RUNNABLE || threads[i].state == THREAD_BLOCKED)) {
      ++count;
    }
  }
  return count;
}

void cell_wake_vfork_parent_of(int child_id) {
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_VFORK, &cursor))) {
    if (thread->wait.target == child_id) {
      cell_finish_wait(thread, child_id);
    }
  }
}

int cell_sleep_current(uint64_t timeout_ticks, struct trap_frame *frame) {
  if (cell_current_thread_internal() == NULL || cell_current_domain_internal() == NULL) { return -EINVAL; }
  if (timeout_ticks == 0) { return 0; }
  return cell_block_current_on_sleep(cell_uptime_ticks() + timeout_ticks, frame);
}

static void wake_timer_waiters(uint64_t scheduler_ticks) {
  for (size_t i = 0; i < thread_slot_limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread->state != THREAD_BLOCKED || !thread->wait.has_deadline || scheduler_ticks < thread->wait.deadline_tick) {
      continue;
    }
    switch (thread->wait.reason) {
    case WAIT_URING: {
      bool ready=uring_poll(&thread->wait.held_file->ring,CELL_POLLIN)!=0;
      int result=thread->wait.uring_submitted;
      cell_finish_wait(thread,result || ready ? result : -62);
      break;
    }
    case WAIT_SLEEP: cell_finish_wait(thread, 0); break;
    case WAIT_FUTEX: cell_finish_wait(thread, -ETIMEDOUT); break;
    case WAIT_PIPE: cell_finish_wait(thread, -11); break;
    default: break;
    }
  }
}

void cell_save_current(const struct trap_frame *frame) {
  struct thread *current_thread = cell_current_thread_internal();
  if (current_thread == NULL) { return; }
  current_thread->tf = *frame;
  arch_save_context(&current_thread->context);
}

static void restore_thread(struct thread *thread, struct trap_frame *frame) {
  vmm_install_user(cell_domain_as(thread->domain));
  arch_restore_context(&thread->context);
  *frame = thread->tf;
}

static void cleanup_reaped_current_domain(struct domain *old_domain, struct thread *next) {
  if (old_domain == NULL || old_domain == next->domain || !old_domain->zombie || old_domain->parent_id != 0) { return; }
  for (size_t i = 0; i < thread_slot_limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == old_domain && thread->running_cpu >= 0) { return; }
  }
  cell_destroy_domain(old_domain);
}

void cell_schedule(struct trap_frame *frame) {
  uint32_t cpu = smp_current_cpu();
  struct thread *current_thread = cell_current_thread_internal();
  struct domain *old_domain = current_thread == NULL ? NULL : current_thread->domain;
  size_t start = current_thread == NULL ? 0 : (size_t)(current_thread - threads) + 1;
  bool saved = false;
  for (;;) {
    bool pending = false;
    size_t slot = start;
    for (size_t n = 0; n < thread_slot_limit; ++n) {
      if (slot >= thread_slot_limit) { slot = 0; }
      struct thread *candidate = &threads[slot++];
      if (candidate->state == THREAD_BLOCKED) { pending = true; }
      if (candidate->state != THREAD_RUNNABLE) { continue; }
      if (candidate->running_cpu >= 0 && candidate->running_cpu != (int)cpu) {
        pending = true;
        continue;
      }
      if (candidate == current_thread && candidate->running_cpu == (int)cpu && !saved) { return; }
      if (!saved) { cell_save_current(frame); }
      if (current_thread != NULL && current_thread->running_cpu == (int)cpu) { current_thread->running_cpu = -1; }
      smp_set_current_thread_slot(cpu, candidate);
      candidate->running_cpu = (int)cpu;
      cleanup_reaped_current_domain(old_domain, candidate);
      restore_thread(candidate, frame);
      return;
    }
    if (!pending) { break; }
    if (!saved) {
      cell_save_current(frame);
      saved = true;
    }
    smp_set_scheduler_waiting(cpu, true);
    smp_kernel_unlock();
    smp_wait_for_irq();
    smp_kernel_lock();
    smp_set_scheduler_waiting(cpu, false);
  }
  kprintf("[kernel] no runnable threads\n");
  arch_poweroff();
}

void cell_timer_tick(struct trap_frame *frame, bool from_lower_el) {
  uint32_t cpu = smp_current_cpu();
  uint64_t ticks = cell_account_tick(cpu, from_lower_el, cell_scheduler_waiting_for_interrupt());
  if (cpu == 0) {
    cell_expire_real_timers(ticks);
    wake_timer_waiters(ticks);
    net_poll();
    cell_socket_timer_tick(ticks);
    cell_wake_poll_waiters_internal();
  }
  struct thread *current = cell_current_thread_internal();
  if (current == NULL || current->domain == NULL) { return; }
  if (from_lower_el) { ++current->cpu_ticks; }
  struct domain *domain = current->domain;
  if (cell_charge_cpu_budget(domain, from_lower_el)) {
    kprintf("[spore] domain %d exceeded CPU budget -> killed\n", domain->id);
    if (from_lower_el) {
      cell_exit_group_current(137, frame);
    } else {
      domain->zombie = true;
      domain->exit_status = 137;
    }
    return;
  }
  if (from_lower_el && cell_deliver_pending_signals_current(frame)) { return; }
  if (from_lower_el) { cell_schedule(frame); }
}
