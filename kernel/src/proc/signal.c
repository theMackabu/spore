#include "proc/signal.h"

#include "kprintf.h"
#include "mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/process.h"
#include "proc/thread.h"

#include <stddef.h>
#include <stdint.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
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
  SIGTERM = 15,
  SIGCHLD = 17,
  SIGCONT = 18,
  SIGURG = 23,
  SIGWINCH = 28,
  SEGV_MAPERR = 1,
  SA_SIGINFO = 4,
  SA_ONSTACK = 0x08000000,
  SS_ONSTACK = 1,
  SS_DISABLE = 2,
  NSIG = 65,
};

static const uint64_t SA_RESETHAND = 0x80000000ull;
static const uint64_t SA_RESTART = 0x10000000ull;

struct k_sigaction64 {
  uint64_t handler;
  uint64_t flags;
  uint64_t restorer;
  uint32_t mask[2];
};

struct siginfo_fields64 {
  int32_t si_signo;
  int32_t si_errno;
  int32_t si_code;
  uint64_t si_addr;
};

struct siginfo64 {
  struct siginfo_fields64 fields;
  uint8_t pad[128 - sizeof(struct siginfo_fields64)];
};

struct stack_t64 {
  uint64_t ss_sp;
  int32_t ss_flags;
  uint32_t pad;
  uint64_t ss_size;
};

struct sigcontext64 {
  uint64_t fault_address;
  uint64_t regs[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t pstate;
  uint8_t pad[8];
  uint8_t reserved[4096];
};

struct ucontext64 {
  uint64_t uc_flags;
  uint64_t uc_link;
  struct stack_t64 uc_stack;
  uint64_t uc_sigmask;
  uint8_t pad[(1024 - 64) / 8];
  uint8_t pad2[8];
  struct sigcontext64 uc_mcontext;
};

struct signal_frame64 {
  struct siginfo64 siginfo;
  struct ucontext64 ucontext;
  uint64_t magic;
  uint64_t signal;
  uint64_t saved_signal_mask;
  struct trap_frame saved;
};

static_assert(offsetof(struct siginfo64, fields.si_addr) == 16, "linux arm64 siginfo si_addr offset");
static_assert(offsetof(struct ucontext64, uc_mcontext) == 176, "linux arm64 ucontext mcontext offset");
static_assert(offsetof(struct sigcontext64, regs) == 8, "linux arm64 sigcontext regs offset");
static_assert(offsetof(struct sigcontext64, sp) == 256, "linux arm64 sigcontext sp offset");
static_assert(offsetof(struct sigcontext64, pc) == 264, "linux arm64 sigcontext pc offset");
static_assert(offsetof(struct sigcontext64, pstate) == 272, "linux arm64 sigcontext pstate offset");

static struct domain *current_domain(void) {
  return cell_current_domain_internal();
}

static bool signal_is_supported(int signal) {
  return signal == SIGHUP || signal == SIGINT || signal == SIGQUIT || signal == SIGABRT || signal == SIGKILL ||
         signal == SIGUSR1 || signal == SIGSEGV || signal == SIGUSR2 || signal == SIGPIPE || signal == SIGTERM ||
         signal == SIGCHLD || signal == SIGCONT || signal == SIGURG || signal == SIGWINCH;
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

static uint64_t signal_mask_sanitized(uint64_t mask) {
  return mask & ~signal_bit(SIGKILL) & ~signal_bit(SIGSEGV);
}

static bool signal_is_blocked(const struct thread *thread, int signal) {
  return thread != NULL && !signal_is_unblockable(signal) && (thread->signal_mask & signal_bit(signal)) != 0;
}

static bool thread_on_sigaltstack(const struct thread *thread) {
  if (thread == NULL || (thread->sigaltstack_flags & SS_DISABLE) != 0 || thread->sigaltstack_size == 0) {
    return false;
  }
  uint64_t sp = thread->tf.sp_el0;
  uint64_t end = thread->sigaltstack_sp + thread->sigaltstack_size;
  return end >= thread->sigaltstack_sp && sp >= thread->sigaltstack_sp && sp < end;
}

static void fill_signal_context(struct signal_frame64 *frame, const struct thread *thread, int signal,
                                uint64_t fault_addr, int sig_code) {
  frame->siginfo.fields.si_signo = signal;
  frame->siginfo.fields.si_code = sig_code;
  frame->siginfo.fields.si_addr = fault_addr;

  frame->ucontext.uc_stack.ss_sp = thread->sigaltstack_sp;
  frame->ucontext.uc_stack.ss_flags = thread_on_sigaltstack(thread) ? SS_ONSTACK : thread->sigaltstack_flags;
  frame->ucontext.uc_stack.ss_size = thread->sigaltstack_size;
  frame->ucontext.uc_sigmask = thread->signal_mask;
  frame->ucontext.uc_mcontext.fault_address = fault_addr;
  for (size_t i = 0; i < 31; ++i) {
    frame->ucontext.uc_mcontext.regs[i] = thread->tf.x[i];
  }
  frame->ucontext.uc_mcontext.sp = thread->tf.sp_el0;
  frame->ucontext.uc_mcontext.pc = thread->tf.elr_el1;
  frame->ucontext.uc_mcontext.pstate = thread->tf.spsr_el1;
}

static void terminate_domain_by_signal(struct domain *domain, int signal) {
  if (domain == NULL || domain->zombie) { return; }
  domain->exit_status = 128 + signal;
  domain->term_signal = signal;
  domain->zombie = true;
  cell_close_all_fds(domain);
  for (size_t i = 0; i < cell_thread_capacity(); ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain) {
      thread->state = THREAD_ZOMBIE;
      if (thread->running_cpu < 0) { thread->running_cpu = -1; }
    }
  }
  cell_wake_parent_of(domain);
}

static bool wait_reason_is_restartable(enum wait_reason reason) {
  return reason == WAIT_CHILD || reason == WAIT_STDIN || reason == WAIT_SOCKET || reason == WAIT_PIPE ||
         reason == WAIT_INOTIFY || reason == WAIT_PTY || reason == WAIT_FUTEX;
}

static bool thread_requires_deferred_signal_frame(const struct thread *thread) {
  return thread != NULL && thread != cell_current_thread_internal() && thread->state == THREAD_RUNNABLE &&
         thread->running_cpu >= 0;
}

static void queue_signal_to_thread(struct thread *thread, int signal) {
  thread->pending_signals |= signal_bit(signal);
  __asm__ volatile("sev" ::: "memory");
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

  if (thread->state == THREAD_BLOCKED) {
    bool restart =
      (action->flags & SA_RESTART) != 0 && wait_reason_is_restartable(thread->wait_reason) && thread->tf.elr_el1 >= 4;
    if (restart) {
      thread->tf.elr_el1 -= 4;
    } else {
      thread->tf.x[0] = (uint64_t)(-(int64_t)EINTR);
    }
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    thread->wait_target = -1;
    thread->stdin_buf = 0;
    thread->stdin_len = 0;
    thread->pipe_buf = 0;
    thread->pipe_len = 0;
    thread->socket_addr = 0;
    thread->socket_addrlen = 0;
    thread->socket_accept_flags = 0;
    thread->socket_write = false;
    thread->socket_msg = false;
    thread->socket_msg_addr = 0;
    thread->socket_iov = 0;
    thread->socket_iovlen = 0;
    thread->socket_flags = 0;
    thread->socket_has_deadline = false;
    thread->socket_deadline_tick = 0;
  }

  struct signal_frame64 frame;
  kmemset(&frame, 0, sizeof(frame));
  frame.magic = 0x5350475349474652ull; // "SPGSIGFR"
  frame.signal = (uint64_t)signal;
  frame.saved_signal_mask = thread->signal_mask;
  frame.saved = thread->tf;
  fill_signal_context(&frame, thread, signal, fault_addr, sig_code);

  uint64_t stack_top = thread->tf.sp_el0;
  if ((action->flags & SA_ONSTACK) != 0 && !thread_on_sigaltstack(thread) &&
      (thread->sigaltstack_flags & SS_DISABLE) == 0 && thread->sigaltstack_size >= sizeof(frame)) {
    stack_top = thread->sigaltstack_sp + thread->sigaltstack_size;
  }
  uint64_t frame_addr = (stack_top - sizeof(frame)) & ~15ull;
  if (!cell_domain_ensure_user_range(domain, frame_addr, sizeof(frame), VMM_ACCESS_WRITE) ||
      !vmm_copy_to_user(cell_domain_as(domain), frame_addr, &frame, sizeof(frame))) {
    terminate_domain_by_signal(domain, SIGSEGV);
    return true;
  }

  uint64_t siginfo_addr = frame_addr + offsetof(struct signal_frame64, siginfo);
  uint64_t ucontext_addr = frame_addr + offsetof(struct signal_frame64, ucontext);
  thread->tf.x[0] = (uint64_t)signal;
  thread->tf.x[1] = (action->flags & SA_SIGINFO) != 0 ? siginfo_addr : 0;
  thread->tf.x[2] = (action->flags & SA_SIGINFO) != 0 ? ucontext_addr : 0;
  thread->tf.x[30] = action->restorer;
  thread->tf.sp_el0 = frame_addr;
  thread->tf.elr_el1 = action->handler;
  thread->signal_mask = signal_mask_sanitized(thread->signal_mask | action->mask | signal_bit(signal));
  if ((action->flags & SA_RESETHAND) != 0) { action->handler = 0; }
  return true;
}

bool cell_deliver_pending_signals(struct thread *thread) {
  if (thread == NULL || thread->pending_signals == 0) { return false; }
  for (int signal = 1; signal < (int)NSIG; ++signal) {
    uint64_t bit = signal_bit(signal);
    if ((thread->pending_signals & bit) == 0 || signal_is_blocked(thread, signal)) { continue; }
    thread->pending_signals &= ~bit;
    return cell_deliver_signal_to_thread(thread, signal);
  }
  return false;
}

bool cell_deliver_pending_signals_current(struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL || frame == NULL || thread->pending_signals == 0) { return false; }
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

int cell_rt_sigaction(int signal, uint64_t act_addr, uint64_t old_addr, uint64_t sigset_size) {
  struct domain *domain = current_domain();
  if (domain == NULL || signal <= 0 || signal >= (int)NSIG || sigset_size != 8) { return -EINVAL; }
  if (old_addr != 0) {
    struct k_sigaction64 old = {
      .handler = domain->signal_actions[signal].handler,
      .flags = domain->signal_actions[signal].flags,
      .restorer = domain->signal_actions[signal].restorer,
    };
    old.mask[0] = (uint32_t)(domain->signal_actions[signal].mask & 0xffffffffu);
    old.mask[1] = (uint32_t)(domain->signal_actions[signal].mask >> 32);
    if (!cell_domain_ensure_user_range(domain, old_addr, sizeof(old), VMM_ACCESS_WRITE) ||
        !vmm_copy_to_user(cell_domain_as(domain), old_addr, &old, sizeof(old))) {
      return -EFAULT;
    }
  }
  if (act_addr != 0) {
    struct k_sigaction64 act;
    if (!cell_domain_ensure_user_range(domain, act_addr, sizeof(act), VMM_ACCESS_READ) ||
        !vmm_copy_from_user(cell_domain_as(domain), &act, act_addr, sizeof(act))) {
      return -EFAULT;
    }
    domain->signal_actions[signal].handler = act.handler;
    domain->signal_actions[signal].flags = act.flags;
    domain->signal_actions[signal].restorer = act.restorer;
    domain->signal_actions[signal].mask = ((uint64_t)act.mask[1] << 32) | act.mask[0];
  }
  return 0;
}

int cell_rt_sigreturn(struct trap_frame *frame) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return -EFAULT; }
  struct signal_frame64 sigframe;
  uint64_t frame_addr = frame->sp_el0;
  if (!cell_domain_ensure_user_range(domain, frame_addr, sizeof(sigframe), VMM_ACCESS_READ) ||
      !vmm_copy_from_user(cell_domain_as(domain), &sigframe, frame_addr, sizeof(sigframe)) ||
      sigframe.magic != 0x5350475349474652ull) {
    cell_signal_current(SIGSEGV, frame);
    return -EFAULT;
  }
  *frame = sigframe.saved;
  for (size_t i = 0; i < 31; ++i) {
    frame->x[i] = sigframe.ucontext.uc_mcontext.regs[i];
  }
  frame->sp_el0 = sigframe.ucontext.uc_mcontext.sp;
  frame->elr_el1 = sigframe.ucontext.uc_mcontext.pc;
  frame->spsr_el1 = sigframe.ucontext.uc_mcontext.pstate;
  struct thread *thread = cell_current_thread_internal();
  if (thread != NULL) {
    thread->tf = *frame;
    thread->signal_mask = signal_mask_sanitized(sigframe.saved_signal_mask);
    (void)cell_deliver_pending_signals_current(frame);
  }
  return 0;
}

void cell_dump_current_fault(const struct trap_frame *frame, uint64_t far) {
  struct domain *domain = current_domain();
  struct thread *thread = cell_current_thread_internal();
  if (domain == NULL || thread == NULL) {
    kprintf("[kernel] fault: no current domain esr=%x elr=%p far=%p\n", frame == NULL ? 0 : (unsigned)frame->esr_el1,
            frame == NULL ? NULL : (void *)(uintptr_t)frame->elr_el1, (void *)(uintptr_t)far);
    return;
  }
  kprintf(
    "[kernel] fault: pid=%d tid=%d cmd=%s cwd=%s esr=%x elr=%p far=%p sp=%p x0=%p x1=%p x2=%p x3=%p x8=%p x16=%p "
    "x17=%p x19=%p x20=%p x21=%p x22=%p x29=%p x30=%p\n",
    domain->id, thread->tid, domain->cmdline[0] != '\0' ? domain->cmdline : domain->name,
    domain->cwd[0] != '\0' ? domain->cwd : "/", frame == NULL ? 0 : (unsigned)frame->esr_el1,
    frame == NULL ? NULL : (void *)(uintptr_t)frame->elr_el1, (void *)(uintptr_t)far,
    frame == NULL ? NULL : (void *)(uintptr_t)frame->sp_el0, frame == NULL ? NULL : (void *)(uintptr_t)frame->x[0],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[1], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[2],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[3], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[8],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[16], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[17],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[19], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[20],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[21], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[22],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[29], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[30]);
  if (frame != NULL) {
    uint64_t words[4] = {0};
    if (vmm_copy_from_user(cell_domain_as(domain), words, frame->sp_el0, sizeof(words))) {
      kprintf("[kernel] fault stack: %p %p %p %p\n", (void *)(uintptr_t)words[0], (void *)(uintptr_t)words[1],
              (void *)(uintptr_t)words[2], (void *)(uintptr_t)words[3]);
    }
    if (frame->x[19] != 0 && vmm_copy_from_user(cell_domain_as(domain), words, frame->x[19], sizeof(words))) {
      kprintf("[kernel] fault x19 mem: %p %p %p %p\n", (void *)(uintptr_t)words[0], (void *)(uintptr_t)words[1],
              (void *)(uintptr_t)words[2], (void *)(uintptr_t)words[3]);
    }
    for (size_t i = 0; i < vma_capacity(cell_domain_vmas(domain)); ++i) {
      const struct vma *vma = vma_at(cell_domain_vmas(domain), i);
      if (vma == NULL || !vma->used) { continue; }
      bool interesting = (frame->sp_el0 >= vma->start && frame->sp_el0 < vma->end) ||
                         (frame->elr_el1 >= vma->start && frame->elr_el1 < vma->end) ||
                         (frame->x[19] >= vma->start && frame->x[19] < vma->end) ||
                         (far >= vma->start && far < vma->end);
      if (interesting) {
        kprintf("[kernel] fault vma: %p-%p prot=%x type=%d\n", (void *)(uintptr_t)vma->start,
                (void *)(uintptr_t)vma->end, (unsigned)vma->prot, (int)vma->type);
      }
    }
  }
}

int cell_kill(int pid, int signal) {
  if (pid == 0 || pid < -1) {
    int pgrp = pid == 0 ? cell_getpgid(0) : -pid;
    int delivered = 0;
    for (size_t i = 0; i < cell_domain_capacity(); ++i) {
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
    for (size_t i = 0; i < cell_thread_capacity(); ++i) {
      struct thread *thread = cell_thread_slot(i);
      if (thread != NULL && thread->state != THREAD_UNUSED && thread->tid == tid) { return 0; }
    }
    return -ESRCH;
  }
  if (!signal_is_supported(signal)) { return 0; }
  for (size_t i = 0; i < cell_thread_capacity(); ++i) {
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
    for (size_t i = 0; i < cell_thread_capacity(); ++i) {
      struct thread *thread = cell_thread_slot(i);
      if (thread != NULL && thread->state != THREAD_UNUSED && thread->tid == tid && thread->domain == domain) {
        return 0;
      }
    }
    return -ESRCH;
  }
  if (!signal_is_supported(signal)) { return 0; }
  for (size_t i = 0; i < cell_thread_capacity(); ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state == THREAD_UNUSED || thread->tid != tid || thread->domain != domain) {
      continue;
    }
    (void)cell_deliver_signal_to_thread(thread, signal);
    return 0;
  }
  return -ESRCH;
}
