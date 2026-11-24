#include "init/app.h"
#include "abi/linux/shared/usercopy.h"
#include "abi/linux/shared/layout.h"
#include "handlers.h"
#include "../time/handlers.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "proc/accounting.h"
#include "proc/domain.h"
#include "proc/futex.h"
#include "proc/process.h"

#include <stdbool.h>
#include <stdint.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
  ENOSYS = 38,
  CLONE_VM = 0x00000100,
  CLONE_FS = 0x00000200,
  CLONE_FILES = 0x00000400,
  CLONE_SIGHAND = 0x00000800,
  CLONE_VFORK = 0x00004000,
  CLONE_THREAD = 0x00010000,
  CLONE_SYSVSEM = 0x00040000,
  CLONE_SETTLS = 0x00080000,
  CLONE_PARENT_SETTID = 0x00100000,
  CLONE_CHILD_CLEARTID = 0x00200000,
  CLONE_DETACHED = 0x00400000,
  CLONE_CHILD_SETTID = 0x01000000,
  FUTEX_WAIT = 0,
  FUTEX_WAKE = 1,
  FUTEX_REQUEUE = 3,
  FUTEX_WAIT_BITSET = 9,
  FUTEX_WAKE_BITSET = 10,
  FUTEX_CLOCK_REALTIME = 256,
  FUTEX_PRIVATE_FLAG = 128,
  FUTEX_CMD_MASK = 127,
};

static bool futex_deadline(struct timespec64 ts, bool absolute, bool realtime, uint64_t *out) {
  if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) { return false; }
  if (absolute) {
    struct timespec64 now = syscall_wall_time(realtime);
    ts.tv_sec -= now.tv_sec;
    ts.tv_nsec -= now.tv_nsec;
    if (ts.tv_nsec < 0) {
      --ts.tv_sec;
      ts.tv_nsec += 1000000000;
    }
  }
  uint64_t now = cell_uptime_ticks();
  if (ts.tv_sec < 0 || (!ts.tv_sec && !ts.tv_nsec)) {
    *out = now;
    return true;
  }
  uint64_t fraction = ((uint64_t)ts.tv_nsec + 9999999) / 10000000;
  *out = UINT64_MAX;
  if ((uint64_t)ts.tv_sec <= (UINT64_MAX - fraction) / 100) {
    uint64_t ticks = (uint64_t)ts.tv_sec * 100 + fraction;
    if (ticks < UINT64_MAX - now) { *out = now + ticks + 1; }
  }
  return true;
}

int64_t sys_clone(struct trap_frame *f, uint64_t flags, uint64_t newsp, uint64_t parent_tid, uint64_t tls,
                  uint64_t child_tid) {
  if (app_mode && (!(flags & CLONE_THREAD) || !(flags & CLONE_VM) || (flags & CLONE_VFORK))) { return -1; }
  const uint64_t signal_mask = 0xff;
  if ((flags & CLONE_VFORK) != 0) {
    const uint64_t allowed_vfork_flags =
      CLONE_VM | CLONE_VFORK | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;
    if ((flags & ~(allowed_vfork_flags | signal_mask)) != 0) { return -(int64_t)ENOSYS; }
    return cell_vfork_current(f, newsp, flags, parent_tid, tls, child_tid);
  }
  if ((flags & CLONE_VM) == 0) {
    if (newsp != 0) { return -(int64_t)ENOSYS; }
    return cell_fork_current(f);
  }

  const uint64_t allowed_thread_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
                                        CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID |
                                        CLONE_CHILD_SETTID | CLONE_DETACHED;
  if ((flags & ~(allowed_thread_flags | signal_mask)) != 0 || newsp == 0) { return -(int64_t)ENOSYS; }
  return cell_clone_thread_current(f, flags, newsp, parent_tid, tls, child_tid);
}

int64_t sys_futex(struct trap_frame *f, uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout, uint64_t uaddr2, uint32_t bitset) {
  uint64_t cmd = op & FUTEX_CMD_MASK;
  bool realtime = (op & FUTEX_CLOCK_REALTIME) != 0;
  if ((op & ~(uint64_t)(FUTEX_CMD_MASK | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)) ||
      (realtime && cmd != FUTEX_WAIT_BITSET)) {
    kprintf("[kernel] futex op %d ENOSYS\n", (int)op);
    return -(int64_t)ENOSYS;
  }
  if (cmd == FUTEX_WAIT || cmd == FUTEX_WAKE) { bitset = UINT32_MAX; }
  if ((cmd == FUTEX_WAIT_BITSET || cmd == FUTEX_WAKE_BITSET) && !bitset) { return -EINVAL; }
  switch (cmd) {
  case FUTEX_WAIT:
  case FUTEX_WAIT_BITSET: {
    bool has_deadline = timeout != 0;
    uint64_t deadline = 0;
    if (has_deadline) {
      struct timespec64 ts;
      if (!syscall_user_readable(timeout, sizeof(ts)) ||
          !vmm_copy_from_user(syscall_active_as(), &ts, timeout, sizeof(ts))) {
        return -(int64_t)EFAULT;
      }
      if (!futex_deadline(ts, cmd == FUTEX_WAIT_BITSET, realtime, &deadline)) { return -EINVAL; }
    }
    return cell_futex_wait_current(uaddr, (uint32_t)val, bitset, has_deadline, deadline, f);
  }
  case FUTEX_REQUEUE:
    return cell_futex_requeue(uaddr, uaddr2, (int32_t)val, (int32_t)timeout);
  case FUTEX_WAKE:
  case FUTEX_WAKE_BITSET:
    return cell_futex_wake_current(uaddr, (uint32_t)val, bitset);
  default:
    kprintf("[kernel] futex op %d ENOSYS\n", (int)op);
    return -(int64_t)ENOSYS;
  }
}
