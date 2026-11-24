#include "proc/signal.h"

#include "arch/aarch64/regs.h"
#include "arch/aarch64/signal_frame.h"
#include "arch/aarch64/smp.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/process.h"
#include "proc/thread.h"

#include <stddef.h>
#include <stdint.h>

enum {
  EINTR = 4,
  ESRCH = 3,
  SIGHUP = 1,
  SIGINT = 2,
  SIGQUIT = 3,
  SIGABRT = 6,
  SIGKILL = 9,
  SIGUSR1 = 10,
  SIGSEGV = 11,
  SIGUSR2 = 12,
  SIGPIPE = 13,
  SIGALRM = 14,
  SIGTERM = 15,
  SIGCHLD = 17,
  SIGCONT = 18,
  SIGURG = 23,
  SIGWINCH = 28,
  NSIG = 65,
};

static const uint64_t SA_RESETHAND = 0x80000000ull;
static const uint64_t SA_RESTART = 0x10000000ull;

static bool signal_is_supported(int signal) {
  return signal == SIGHUP || signal == SIGINT || signal == SIGQUIT || signal == SIGABRT || signal == SIGKILL ||
         signal == SIGUSR1 || signal == SIGSEGV || signal == SIGUSR2 || signal == SIGPIPE || signal == SIGALRM ||
         signal == SIGTERM || signal == SIGCHLD || signal == SIGCONT || signal == SIGURG || signal == SIGWINCH;
}

static bool signal_default_ignored(int signal) {
  return signal == SIGCHLD || signal == SIGCONT || signal == SIGURG || signal == SIGWINCH;
}

static uint64_t signal_bit(int signal) {
  if (signal <= 0 || signal >= (int)NSIG) { return 0; }
  return 1ull << (uint64_t)(signal - 1);
}

static bool signal_is_unblockable(int signal) {
  return signal == SIGKILL || signal == SIGSEGV;
}

uint64_t cell_sanitize_signal_mask(uint64_t mask) {
  return mask & ~signal_bit(SIGKILL) & ~signal_bit(SIGSEGV);
}

static bool signal_is_blocked(const struct thread *thread, int signal) {
  return thread != NULL && !signal_is_unblockable(signal) && (thread->signal_mask & signal_bit(signal)) != 0;
}

static void terminate_domain_by_signal(struct domain *domain, int signal) {
  if (domain == NULL || domain->zombie) { return; }
  domain->exit_status = 128 + signal;
  domain->term_signal = signal;
  domain->zombie = true;
  cell_close_all_fds(domain);
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain) {
      cell_clear_wait(thread);
      thread->state = THREAD_ZOMBIE;
      if (thread->running_cpu < 0) { thread->running_cpu = -1; }
    }
  }
  cell_wake_parent_of(domain);
}

static bool wait_is_restartable(const struct thread_wait *wait) {
  enum wait_reason reason = wait->reason;
  if (reason == WAIT_FUTEX) { return !wait->has_deadline; }
  return reason == WAIT_CHILD || reason == WAIT_STDIN || reason == WAIT_SOCKET || reason == WAIT_PIPE ||
         reason == WAIT_INOTIFY || reason == WAIT_PTY || reason == WAIT_FLOCK;
}

static bool thread_requires_deferred_signal_frame(const struct thread *thread) {
  return thread != NULL && thread != cell_current_thread_internal() && thread->state == THREAD_RUNNABLE &&
         thread->running_cpu >= 0;
}

static void queue_signal_to_thread(struct thread *thread, int signal) {
  thread->pending_signals |= signal_bit(signal);
  smp_notify();
}

bool cell_deliver_signal_to_thread(struct thread *thread, int signal) {
  return cell_deliver_signal_to_thread_fault(thread, signal, 0, 0);
}

bool cell_deliver_signal_to_thread_fault(struct thread *thread, int signal, uint64_t fault_addr, int sig_code) {
  if (thread == NULL || thread->domain == NULL || signal <= 0 || signal >= (int)NSIG) { return false; }
  if (signal_is_blocked(thread, signal)) {
    queue_signal_to_thread(thread, signal);
    return false;
  }
  struct domain *domain = thread->domain;
  struct signal_action *action = &domain->signal_actions[signal];
  if (action->handler == 0 && signal_default_ignored(signal)) { return true; }
  if (action->handler == 0 || signal == SIGKILL) {
    terminate_domain_by_signal(domain, signal);
    return true;
  }
  if (action->handler == 1) { return true; }
  if (action->restorer == 0) {
    terminate_domain_by_signal(domain, signal);
    return true;
  }
  if (thread_requires_deferred_signal_frame(thread)) {
    queue_signal_to_thread(thread, signal);
    return false;
  }

  uint64_t handler_mask = thread->signal_mask;
  if (thread->state == THREAD_BLOCKED) {
    if ((action->flags & SA_RESTART) && wait_is_restartable(&thread->wait) &&
        aarch64_restart_syscall(&thread->tf)) {
      cell_resume_wait(thread);
    } else {
      cell_finish_wait(thread, -EINTR);
    }
  }

  if (!aarch64_push_signal_frame(thread, action, signal, fault_addr, sig_code)) {
    terminate_domain_by_signal(domain, SIGSEGV);
    return true;
  }
  thread->signal_mask = cell_sanitize_signal_mask(handler_mask | action->mask | signal_bit(signal));
  if ((action->flags & SA_RESETHAND) != 0) { action->handler = 0; }
  return true;
}

void cell_signal_domain(struct domain *domain, int signal, struct trap_frame *frame) {
  struct thread *target = NULL;
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *t = cell_thread_slot(i);
    if (t == NULL || t->domain != domain || t->state == THREAD_UNUSED || t->state == THREAD_ZOMBIE ||
        signal_is_blocked(t, signal)) {
      continue;
    }
    if (target == NULL || t->state == THREAD_BLOCKED) { target = t; }
    if (t->state == THREAD_BLOCKED && t->wait.reason == WAIT_STDIN) { break; }
  }
  if (target == NULL) {
    domain->pending_signals |= signal_bit(signal);
  } else if (target == cell_current_thread_internal() && frame != NULL) {
    cell_signal_current(signal, frame);
  } else if (target->state == THREAD_RUNNABLE && target->running_cpu >= 0) {
    queue_signal_to_thread(target, signal);
  } else {
    cell_deliver_signal_to_thread(target, signal);
  }
}

void cell_expire_real_timers(uint64_t now) {
  for (size_t i = 0, limit = cell_domain_slot_limit(); i < limit; ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (!domain->used || domain->zombie || !domain->alarm_deadline || now < domain->alarm_deadline) { continue; }
    uint64_t interval = domain->alarm_interval;
    domain->alarm_deadline = interval && interval <= UINT64_MAX - now ? now + interval : 0;
    cell_signal_domain(domain, SIGALRM, NULL);
  }
}

bool cell_deliver_pending_signals(struct thread *thread) {
  if (thread == NULL || thread->domain == NULL || (thread->pending_signals | thread->domain->pending_signals) == 0) {
    return false;
  }
  for (int signal = 1; signal < (int)NSIG; ++signal) {
    uint64_t bit = signal_bit(signal);
    uint64_t *pending = (thread->pending_signals & bit) ? &thread->pending_signals : &thread->domain->pending_signals;
    if ((*pending & bit) == 0 || signal_is_blocked(thread, signal)) { continue; }
    *pending &= ~bit;
    const struct signal_action *action = &thread->domain->signal_actions[signal];
    if (signal != SIGKILL && (action->handler == 1 || (action->handler == 0 && signal_default_ignored(signal)))) {
      continue;
    }
    return cell_deliver_signal_to_thread(thread, signal);
  }
  return false;
}

bool cell_deliver_pending_signals_current(struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL || frame == NULL || thread->domain == NULL ||
      (thread->pending_signals | thread->domain->pending_signals) == 0) {
    return false;
  }
  thread->tf = *frame;
  bool delivered = cell_deliver_pending_signals(thread);
  *frame = thread->tf;
  if (thread->state == THREAD_ZOMBIE) {
    cell_schedule(frame);
    return true;
  }
  return delivered;
}

bool cell_signal_current(int signal, struct trap_frame *frame) {
  return cell_signal_current_fault(signal, frame, 0, 0);
}

bool cell_signal_current_fault(int signal, struct trap_frame *frame, uint64_t fault_addr, int sig_code) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL || frame == NULL) { return false; }
  bool ignored = thread->domain != NULL && signal > 0 && signal < (int)NSIG &&
                 thread->domain->signal_actions[signal].handler == 1 && signal != SIGKILL;
  thread->tf = *frame;
  bool delivered = cell_deliver_signal_to_thread_fault(thread, signal, fault_addr, sig_code);
  *frame = thread->tf;
  if (thread->state == THREAD_ZOMBIE) {
    cell_schedule(frame);
    return true;
  }
  return delivered && !ignored;
}

int cell_kill(int pid, int signal) {
  if (pid == 0 || pid < -1) {
    int pgrp = pid == 0 ? cell_getpgid(0) : -pid;
    int delivered = 0;
    for (size_t i = 0, limit = cell_domain_slot_limit(); i < limit; ++i) {
      struct domain *domain = cell_domain_slot(i);
      if (domain != NULL && domain->used && !domain->zombie && domain->pgrp_id == pgrp) {
        if (signal == 0) {
          ++delivered;
          continue;
        }
        if (!signal_is_supported(signal)) {
          ++delivered;
          continue;
        }
        (void)cell_deliver_signal_to_thread(cell_thread_for_domain(domain), signal);
        ++delivered;
      }
    }
    return delivered == 0 ? -ESRCH : 0;
  }
  struct domain *domain = cell_find_domain(pid);
  if (domain == NULL || domain->zombie) { return -ESRCH; }
  if (signal == 0) { return 0; }
  if (!signal_is_supported(signal)) { return 0; }
  (void)cell_deliver_signal_to_thread(cell_thread_for_domain(domain), signal);
  return 0;
}

int cell_tkill(int tid, int signal) {
  if (signal == 0) {
    for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
      struct thread *thread = cell_thread_slot(i);
      if (thread != NULL && thread->state != THREAD_UNUSED && thread->tid == tid) { return 0; }
    }
    return -ESRCH;
  }
  if (!signal_is_supported(signal)) { return 0; }
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state == THREAD_UNUSED || thread->tid != tid || thread->domain == NULL) { continue; }
    (void)cell_deliver_signal_to_thread(thread, signal);
    return 0;
  }
  return -ESRCH;
}

int cell_tgkill(int pid, int tid, int signal) {
  struct domain *domain = cell_find_domain(pid);
  if (domain == NULL || domain->zombie) { return -ESRCH; }
  if (signal == 0) {
    for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
      struct thread *thread = cell_thread_slot(i);
      if (thread != NULL && thread->state != THREAD_UNUSED && thread->tid == tid && thread->domain == domain) {
        return 0;
      }
    }
    return -ESRCH;
  }
  if (!signal_is_supported(signal)) { return 0; }
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state == THREAD_UNUSED || thread->tid != tid || thread->domain != domain) {
      continue;
    }
    (void)cell_deliver_signal_to_thread(thread, signal);
    return 0;
  }
  return -ESRCH;
}
