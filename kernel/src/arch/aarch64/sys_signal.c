#include "arch/aarch64/syscall_handlers.h"

#include "proc/signal.h"
#include "proc/thread.h"

#include <stdint.h>

enum {
  ENOMEM = 12,
  EFAULT = 14,
  EINVAL = 22,
  EPERM = 1,
  SIG_BLOCK = 0,
  SIG_UNBLOCK = 1,
  SIG_SETMASK = 2,
  SIGKILL = 9,
  SIGSEGV = 11,
  SS_ONSTACK = 1,
  SS_DISABLE = 2,
  MINSIGSTKSZ = 2048,
};

struct stack_t64 {
  uint64_t ss_sp;
  int32_t ss_flags;
  uint32_t _pad;
  uint64_t ss_size;
};

static bool on_sigaltstack(const struct thread *thread, const struct trap_frame *frame) {
  if (thread == NULL || frame == NULL || (thread->sigaltstack_flags & SS_DISABLE) != 0 ||
      thread->sigaltstack_size == 0) {
    return false;
  }
  uint64_t end = thread->sigaltstack_sp + thread->sigaltstack_size;
  return end >= thread->sigaltstack_sp && frame->sp_el0 >= thread->sigaltstack_sp && frame->sp_el0 < end;
}

int64_t sys_sigaltstack(struct trap_frame *frame, uint64_t new_addr, uint64_t old_addr) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL) { return -(int64_t)EFAULT; }
  bool onstack = on_sigaltstack(thread, frame);
  if (old_addr != 0) {
    struct stack_t64 old = {
      .ss_sp = thread->sigaltstack_sp,
      .ss_flags = onstack ? SS_ONSTACK : thread->sigaltstack_flags,
      .ss_size = thread->sigaltstack_size,
    };
    if (!syscall_user_writable(old_addr, sizeof(old)) ||
        !vmm_copy_to_user(syscall_active_as(), old_addr, &old, sizeof(old))) {
      return -(int64_t)EFAULT;
    }
  }
  if (new_addr != 0) {
    if (onstack) { return -(int64_t)EPERM; }
    struct stack_t64 next;
    if (!syscall_user_readable(new_addr, sizeof(next)) ||
        !vmm_copy_from_user(syscall_active_as(), &next, new_addr, sizeof(next))) {
      return -(int64_t)EFAULT;
    }
    if ((next.ss_flags & ~(uint32_t)SS_DISABLE) != 0) { return -(int64_t)EINVAL; }
    if ((next.ss_flags & SS_DISABLE) == 0 && next.ss_size < MINSIGSTKSZ) { return -(int64_t)ENOMEM; }
    thread->sigaltstack_sp = (next.ss_flags & SS_DISABLE) != 0 ? 0 : next.ss_sp;
    thread->sigaltstack_size = (next.ss_flags & SS_DISABLE) != 0 ? 0 : next.ss_size;
    thread->sigaltstack_flags = (next.ss_flags & SS_DISABLE) != 0 ? SS_DISABLE : 0;
  }
  return 0;
}

int64_t sys_rt_sigprocmask(struct trap_frame *frame, uint64_t how, uint64_t set, uint64_t oldset,
                           uint64_t sigsetsize) {
  if (sigsetsize != sizeof(uint64_t)) { return -(int64_t)EINVAL; }
  if (set != 0 && how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) { return -(int64_t)EINVAL; }
  uint64_t next = 0;
  if (set != 0) {
    if (!syscall_user_readable(set, sizeof(uint64_t)) ||
        !vmm_copy_from_user(syscall_active_as(), &next, set, sizeof(next))) {
      return -(int64_t)EFAULT;
    }
  }
  struct thread *thread = cell_current_thread_internal();
  uint64_t old = thread == NULL ? 0 : thread->signal_mask;
  if (oldset != 0) {
    if (!syscall_user_writable(oldset, sizeof(old)) ||
        !vmm_copy_to_user(syscall_active_as(), oldset, &old, sizeof(old))) {
      return -(int64_t)EFAULT;
    }
  }
  if (thread != NULL && set != 0) {
    uint64_t mask = thread->signal_mask;
    if (how == SIG_BLOCK) mask |= next;
    else if (how == SIG_UNBLOCK) mask &= ~next;
    else mask = next;
    mask &= ~(1ull << (SIGKILL - 1));
    mask &= ~(1ull << (SIGSEGV - 1));
    thread->signal_mask = mask;
    if (thread->pending_signals != 0 && frame != NULL) {
      frame->x[0] = 0;
      thread->tf = *frame;
      if (cell_deliver_pending_signals(thread)) {
        *frame = thread->tf;
        return CELL_SWITCHED;
      }
    }
  }
  return 0;
}
