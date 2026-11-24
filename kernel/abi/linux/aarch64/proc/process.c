#include "../usercopy.h"
#include "abi/linux/aarch64/layout.h"
#include "arch/aarch64/regs.h"
#include "handlers.h"
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
  FUTEX_PRIVATE_FLAG = 128,
  FUTEX_CMD_MASK = 127,
};

static bool futex_timeout_ticks(const struct timespec64 *timeout, uint64_t *out) {
  if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000) { return false; }
  uint64_t fractional = ((uint64_t)timeout->tv_nsec * 100 + 999999999ull) / 1000000000ull;
  if ((uint64_t)timeout->tv_sec > (UINT64_MAX - fractional) / 100) {
    *out = UINT64_MAX;
    return true;
  }
  *out = (uint64_t)timeout->tv_sec * 100 + fractional;
  return true;
}

int64_t sys_clone(struct trap_frame *f, uint64_t flags, uint64_t newsp, uint64_t parent_tid, uint64_t tls,
                  uint64_t child_tid) {
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

int64_t sys_futex(struct trap_frame *f, uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout) {
  uint64_t cmd = op & FUTEX_CMD_MASK;
  if ((op & ~(uint64_t)(FUTEX_CMD_MASK | FUTEX_PRIVATE_FLAG)) != 0) {
    kprintf("[kernel] futex op %d ENOSYS\n", (int)op);
    return -(int64_t)ENOSYS;
  }
  switch (cmd) {
  case FUTEX_WAIT: {
    bool has_deadline = timeout != 0;
    uint64_t deadline = 0;
    if (has_deadline) {
      struct timespec64 relative;
      if (!syscall_user_readable(timeout, sizeof(relative)) ||
          !vmm_copy_from_user(syscall_active_as(), &relative, timeout, sizeof(relative))) {
        return -(int64_t)EFAULT;
      }
      uint64_t ticks = 0;
      if (!futex_timeout_ticks(&relative, &ticks)) { return -(int64_t)EINVAL; }
      uint64_t now = cell_uptime_ticks();
      if (ticks == 0) {
        deadline = now;
      } else {
        deadline = ticks >= UINT64_MAX - now ? UINT64_MAX : now + ticks + 1;
      }
    }
    return cell_futex_wait_current(uaddr, (uint32_t)val, has_deadline, deadline, f);
  }
  case FUTEX_WAKE:
    return cell_futex_wake_current(uaddr, (uint32_t)val);
  default:
    kprintf("[kernel] futex op %d ENOSYS\n", (int)op);
    return -(int64_t)ENOSYS;
  }
}
