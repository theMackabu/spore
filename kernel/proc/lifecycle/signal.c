#include "abi/linux/shared/signal.h"
#include "init/app.h"
#include "mm/pool.h"
#include "proc/pid_namespace.h"
#include "proc/signal.h"
#include "proc/signalfd.h"

#include "arch/regs.h"
#include "arch/signal_frame.h"
#include "arch/smp.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "proc/user_namespace.h"

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
  SIGPWR = 30,
  NSIG = 65,
};

static const uint64_t SA_NODEFER = 0x40000000ull;
static const uint64_t SA_RESETHAND = 0x80000000ull;
static const uint64_t SA_RESTART = 0x10000000ull;

bool cell_user_signal_supported(int signal) {
  return signal == SIGHUP || signal == SIGINT || signal == SIGQUIT || signal == SIGABRT || signal == SIGKILL ||
         signal == SIGUSR1 || signal == SIGSEGV || signal == SIGUSR2 || signal == SIGPIPE || signal == SIGALRM ||
         signal == SIGTERM || signal == SIGCHLD || signal == SIGCONT || signal == SIGURG || signal == SIGWINCH ||
         signal == SIGPWR;
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
  if (app_mode) { app_finish(APP_SIGNAL, signal); }
  cell_reparent_children(domain, 0, 0);
  cell_wake_parent_of(domain);
}

void cell_terminate_current(int signal, struct trap_frame *frame) {
  terminate_domain_by_signal(cell_current_domain_internal(), signal);
  cell_schedule(frame);
}

static bool wait_is_restartable(const struct thread_wait *wait) {
  enum wait_reason reason = wait->reason;
  if (reason == WAIT_FUTEX) { return !wait->has_deadline; }
  return reason == WAIT_CHILD || reason == WAIT_STDIN || reason == WAIT_SOCKET || reason == WAIT_PIPE ||
         reason == WAIT_FIFO_OPEN || reason == WAIT_INOTIFY || reason == WAIT_PTY || reason == WAIT_FLOCK ||
         reason == WAIT_EVENTFD || reason == WAIT_SIGNALFD;
}

static bool thread_requires_deferred_signal_frame(const struct thread *thread) {
  return thread != NULL && thread != cell_current_thread_internal() && thread->state == THREAD_RUNNABLE &&
         thread->running_cpu >= 0;
}

static void queue_signal_to_thread(struct thread *thread, int signal) {
  thread->pending_signals |= signal_bit(signal);
  cell_signalfd_notify(thread->domain);
  smp_notify();
}

bool cell_deliver_signal_to_thread(struct thread *thread, int signal) {
  return cell_deliver_signal_to_thread_fault(thread, signal, 0, 0);
}

static int sigwait_result(struct thread *thread, uint64_t addr, struct siginfo_fields64 fields) {
  struct siginfo64 info = {.fields = fields};
  if (addr && (!cell_domain_ensure_user_range(thread->domain, addr, sizeof(info), VMM_ACCESS_WRITE) ||
               !vmm_copy_to_user(cell_domain_as(thread->domain), addr, &info, sizeof(info)))) {
    return -14;
  }
  return fields.si_signo;
}

int cell_take_waited_signal(struct thread *thread, uint64_t mask, uint64_t addr) {
  uint64_t available = thread->pending_signals & mask;
  if (!available) { available = thread->domain->pending_signals & mask; }
  if (!available) { return -11; }
  int signal = __builtin_ctzll(available) + 1;
  return sigwait_result(thread, addr, cell_pending_signal_info(thread, signal, true));
}

static bool waiting_for(const struct thread *t, int signal) {
  return t->state == THREAD_BLOCKED && t->wait.reason == WAIT_SIGWAIT && (t->wait.sigwait_set & signal_bit(signal));
}

static bool deliver_signal(struct thread *thread, struct siginfo_fields64 info) {
  int signal = info.si_signo;
  if (thread == NULL || thread->domain == NULL || signal <= 0 || signal >= (int)NSIG) { return false; }
  if (waiting_for(thread, signal)) {
    cell_finish_wait(thread, sigwait_result(thread, thread->wait.sigwait_info, info));
    smp_notify();
    return true;
  }
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
  if (thread_requires_deferred_signal_frame(thread)) {
    queue_signal_to_thread(thread, signal);
    return false;
  }

  uint64_t handler_mask = thread->signal_mask;
  if (thread->state == THREAD_BLOCKED) {
    if ((action->flags & SA_RESTART) && wait_is_restartable(&thread->wait) && arch_restart_syscall(&thread->tf)) {
      cell_resume_wait(thread);
    } else {
      int64_t result = -EINTR;
      if (thread->wait.reason == WAIT_URING && thread->wait.uring_submitted) { result = thread->wait.uring_submitted; }
      cell_finish_wait(thread, result);
    }
  }

  if (!arch_push_signal_frame(thread, action, &info)) {
    terminate_domain_by_signal(domain, SIGSEGV);
    return true;
  }
  thread->signal_mask =
    cell_sanitize_signal_mask(handler_mask | action->mask | ((action->flags & SA_NODEFER) ? 0 : signal_bit(signal)));
  if ((action->flags & SA_RESETHAND) != 0) { action->handler = 0; }
  return true;
}

bool cell_deliver_signal_to_thread_fault(struct thread *thread, int signal, uint64_t fault_addr, int sig_code) {
  return deliver_signal(thread,
                        (struct siginfo_fields64){.si_signo = signal, .si_code = sig_code, .si_addr = fault_addr});
}

static struct thread *signal_target(struct domain *domain, int signal) {
  struct thread *target = NULL;
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *t = cell_thread_slot(i);
    if (!t || t->domain != domain || t->state == THREAD_UNUSED || t->state == THREAD_ZOMBIE) { continue; }
    if (waiting_for(t, signal)) { return t; }
    if (signal_is_blocked(t, signal)) { continue; }
    if (target == NULL || t->state == THREAD_BLOCKED) { target = t; }
    if (t->state == THREAD_BLOCKED && t->wait.reason == WAIT_STDIN) { break; }
  }
  return target;
}

void cell_signal_domain(struct domain *domain, int signal, struct trap_frame *frame) {
  struct thread *target = signal_target(domain, signal);
  if (target == NULL) {
    domain->pending_signals |= signal_bit(signal);
    cell_signalfd_notify(domain);
  } else if (target == cell_current_thread_internal() && frame != NULL) {
    cell_signal_current(signal, frame);
  } else if (target->state == THREAD_RUNNABLE && target->running_cpu >= 0) {
    queue_signal_to_thread(target, signal);
  } else {
    cell_deliver_signal_to_thread(target, signal);
  }
}

struct parent_notifications {
  uint64_t queued;
  struct signal_sender sender[64];
};

static struct object_pool notification_pool;

int cell_set_parent_death_signal(struct thread *thread, uint64_t signal) {
  if (signal >= NSIG) { return -22; }
  if (signal && !thread->domain->parent_notifications) {
    if (!notification_pool.limit && !pool_init(&notification_pool, sizeof(struct parent_notifications), MAX_DOMAINS)) {
      return -12;
    }
    thread->domain->parent_notifications = pool_alloc(&notification_pool);
    if (!thread->domain->parent_notifications) { return -12; }
  }
  thread->parent_death_signal = signal;
  return 0;
}

void cell_free_parent_notifications(struct domain *domain) {
  if (domain->parent_notifications) {
    pool_free(&notification_pool, domain->parent_notifications);
    domain->parent_notifications = NULL;
  }
}

static void dispatch_parent_notifications(void) {
  static bool draining;
  if (draining) { return; }
  draining = true;
  bool dispatched;
  do {
    dispatched = false;
    for (size_t i = 0; i < cell_domain_slot_limit(); ++i) {
      struct domain *domain = cell_domain_slot(i);
      while (domain->used && !domain->zombie && domain->parent_notifications && domain->parent_notifications->queued) {
        int signal = __builtin_ctzll(domain->parent_notifications->queued) + 1;
        uint64_t bit = signal_bit(signal);
        domain->parent_notifications->queued &= ~bit;
        struct thread *target = signal_target(domain, signal);
        if (!target || target->running_cpu >= 0) {
          domain->pending_signals |= bit;
          cell_signalfd_notify(domain);
          smp_notify();
        } else {
          struct signal_sender sender = domain->parent_notifications->sender[signal - 1];
          domain->parent_notifications->sender[signal - 1] = (struct signal_sender){0};
          deliver_signal(target, (struct siginfo_fields64){.si_signo = signal, .sender = sender});
        }
        dispatched = true;
      }
    }
  } while (dispatched);
  draining = false;
}

void cell_reparent_children(struct domain *parent, int exiting_tid, int survivor_tid) {
  if (!survivor_tid && !exiting_tid && pidns_mark_dead(parent)) {
    static bool sweeping;
    if (!sweeping) {
      sweeping = true;
      bool killed;
      do {
        killed = false;
        for (size_t i = 0; i < cell_domain_slot_limit(); ++i) {
          struct domain *d = cell_domain_slot(i);
          if (d->used && !d->zombie && pidns_has_dead_parent(d)) {
            terminate_domain_by_signal(d, SIGKILL);
            killed = true;
          }
        }
      } while (killed);
      sweeping = false;
    }
  }
  struct domain *init = survivor_tid ? parent : cell_find_domain(pidns_reaper(parent));
  int adopt_pid = init && !init->zombie ? init->id : 0;
  int adopt_tid = survivor_tid ? survivor_tid : adopt_pid;
  bool queued = false;
  for (size_t i = 0; i < cell_domain_slot_limit(); ++i) {
    struct domain *child = cell_domain_slot(i);
    if (!child->used || child->parent_id != parent->id || (exiting_tid && child->parent_tid != exiting_tid)) {
      continue;
    }
    if (!survivor_tid && pidns_has_dead_parent(parent) && pidns_has_dead_parent(child)) {
      child->parent_id = child->parent_tid = 0;
      cell_reap_namespace_orphan(child);
      continue;
    }
    child->parent_id = adopt_pid;
    child->parent_tid = adopt_tid;
    if (!survivor_tid) { child->exit_signal = SIGCHLD; }
    if (child->zombie) {
      if (!survivor_tid && adopt_pid) { cell_wake_parent_of(child); }
      continue;
    }
    if (!child->parent_notifications) { continue; }
    for (size_t j = 0; j < cell_thread_slot_limit(); ++j) {
      struct thread *t = cell_thread_slot(j);
      if (!t || t->domain != child || t->state == THREAD_UNUSED || t->state == THREAD_ZOMBIE ||
          !t->parent_death_signal) {
        continue;
      }
      unsigned signal = t->parent_death_signal;
      uint64_t bit = signal_bit(signal);
      if ((child->pending_signals | child->parent_notifications->queued) & bit) { continue; }
      child->parent_notifications->sender[signal - 1] = (struct signal_sender){
        pidns_local(pidns_active(child), parent->id), userns_visible(child->user_ns, false, parent->uid)};
      child->parent_notifications->queued |= bit;
      queued = true;
    }
  }
  if (queued) { dispatch_parent_notifications(); }
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

struct siginfo_fields64 cell_pending_signal_info(struct thread *thread, int signal, bool consume) {
  uint64_t bit = signal_bit(signal);
  struct domain *domain = thread->domain;
  struct siginfo_fields64 info = {.si_signo = signal};
  if (thread->pending_signals & bit) {
    if (consume) { thread->pending_signals &= ~bit; }
  } else {
    if (signal == SIGCHLD && domain->pending_child.si_signo) {
      info = domain->pending_child;
      if (consume) { domain->pending_child.si_signo = 0; }
    } else if (domain->parent_notifications) {
      info.sender = domain->parent_notifications->sender[signal - 1];
      if (consume) { domain->parent_notifications->sender[signal - 1] = (struct signal_sender){0}; }
    }
    if (consume) { domain->pending_signals &= ~bit; }
  }
  return info;
}

void cell_signal_child_exit(struct domain *parent, struct siginfo_fields64 info) {
  struct thread *target = signal_target(parent, info.si_signo);
  if (target && target->running_cpu < 0) {
    deliver_signal(target, info);
    return;
  }
  uint64_t bit = signal_bit(info.si_signo);
  if (!(parent->pending_signals & bit)) {
    parent->pending_child = info;
    parent->pending_signals |= bit;
    cell_signalfd_notify(parent);
    smp_notify();
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
    struct siginfo_fields64 info = cell_pending_signal_info(thread, signal, true);
    const struct signal_action *action = &thread->domain->signal_actions[signal];
    if (signal != SIGKILL && (action->handler == 1 || (action->handler == 0 && signal_default_ignored(signal)))) {
      continue;
    }
    return deliver_signal(thread, info);
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
  if (sig_code > 0 && thread->domain && signal > 0 && signal < (int)NSIG &&
      (signal_is_blocked(thread, signal) || thread->domain->signal_actions[signal].handler == 1)) {
    terminate_domain_by_signal(thread->domain, signal);
    cell_schedule(frame);
    return true;
  }
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

static bool may_signal(const struct domain *target, int signal) {
  const struct domain *sender = cell_current_domain_internal();
  return sender == target || cell_capable(CAP_KILL) || sender->uid == target->uid || sender->uid == target->saved_uid ||
         sender->euid == target->uid || sender->euid == target->saved_uid ||
         (signal == SIGCONT && sender->session_id == target->session_id);
}

static int send_user_signal(struct domain *victim, struct thread *thread, int signal) {
  if (!victim) { return -ESRCH; }
  if (!may_signal(victim, signal)) { return -1; }
  if (victim->zombie) { return 0; }
  if (signal && pidns_init_process(victim) && victim->signal_actions[signal].handler <= 1 &&
      !(signal == SIGKILL && pidns_active(victim) != pidns_current())) {
    return 0;
  }
  if (cell_user_signal_supported(signal)) {
    if (thread) { (void)cell_deliver_signal_to_thread(thread, signal); }
    else { cell_signal_domain(victim, signal, NULL); }
  }
  return 0;
}

int cell_kill(int pid, int signal) {
  if ((unsigned)signal >= NSIG) { return -22; }
  if (pid <= 0) {
    if (pid == INT32_MIN) { return -ESRCH; }
    int pgrp = pid == 0 ? cell_current_domain_internal()->pgrp_id : pidns_global(pidns_current(), -pid);
    int result = -ESRCH;
    for (size_t i = 0, limit = cell_domain_slot_limit(); i < limit; ++i) {
      struct domain *domain = cell_domain_slot(i);
      if (!domain || !domain->used || !pidns_self_view(domain->id)) { continue; }
      if (pid == -1 ? (domain == cell_current_domain_internal() || pidns_self_view(domain->id) == 1)
                    : domain->pgrp_id != pgrp) {
        continue;
      }
      int rc = send_user_signal(domain, NULL, signal);
      if (result != 0) { result = rc; }
    }
    return result;
  }
  struct domain *domain = cell_find_domain(pidns_global(pidns_current(), pid));
  return send_user_signal(domain, NULL, signal);
}

int cell_tkill(int tid, int signal) {
  if ((unsigned)signal >= NSIG) { return -22; }
  struct thread *thread = cell_find_thread(pidns_global(pidns_current(), tid));
  return send_user_signal(thread ? thread->domain : NULL, thread, signal);
}

int cell_tgkill(int pid, int tid, int signal) {
  if ((unsigned)signal >= NSIG) { return -22; }
  struct thread *thread = cell_find_thread(pidns_global(pidns_current(), tid));
  if (!thread || !thread->domain || pidns_self_view(thread->domain->id) != pid) { return -ESRCH; }
  return send_user_signal(thread->domain, thread, signal);
}
