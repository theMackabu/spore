#include "arch/aarch64/regs.h"
#include "handlers.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/futex.h"
#include "proc/process.h"

#include <stdint.h>

enum {
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
  case FUTEX_WAIT:
    if (timeout != 0) {
      kprintf("[kernel] futex wait timeout ENOSYS\n");
      return -(int64_t)ENOSYS;
    }
    return cell_futex_wait_current(uaddr, (uint32_t)val, f);
  case FUTEX_WAKE:
    return cell_futex_wake_current(uaddr, (uint32_t)val);
  default:
    kprintf("[kernel] futex op %d ENOSYS\n", (int)op);
    return -(int64_t)ENOSYS;
  }
}

int64_t sys_getres_ids(bool group, uint64_t real, uint64_t effective, uint64_t saved) {
  struct domain *d = cell_current_domain_internal();
  if (d == NULL) { return -3; }
  uint32_t ids[] = {group ? d->gid : d->uid, group ? d->egid : d->euid, group ? d->saved_gid : d->saved_uid};
  uint64_t addresses[] = {real, effective, saved};
  for (unsigned i = 0; i < 3; ++i) {
    if (!syscall_user_writable(addresses[i], sizeof(ids[i])) ||
        !vmm_copy_to_user(syscall_active_as(), addresses[i], &ids[i], sizeof(ids[i]))) { return -14; }
  }
  return 0;
}

int64_t sys_getgroups(int count, uint64_t list) {
  struct domain *d = cell_current_domain_internal();
  if (d == NULL) { return -3; }
  if (count < 0 || (count && (unsigned)count < d->group_count)) { return -22; }
  size_t bytes = d->group_count * sizeof(d->groups[0]);
  if (count && bytes && (!syscall_user_writable(list, bytes) ||
      !vmm_copy_to_user(syscall_active_as(), list, d->groups, bytes))) { return -14; }
  return d->group_count;
}

int64_t sys_setgroups(uint64_t count, uint64_t list) {
  struct domain *d = cell_current_domain_internal();
  if (d == NULL || d->euid != 0) { return -1; }
  if (count > DOMAIN_GROUP_LIMIT) { return -22; }
  uint32_t groups[DOMAIN_GROUP_LIMIT];
  size_t bytes = count * sizeof(groups[0]);
  if (bytes && !vmm_copy_from_user(syscall_active_as(), groups, list, bytes)) { return -14; }
  for (size_t i = 0; i < count; ++i) {
    if (groups[i] == UINT32_MAX) { return -22; }
  }
  kmemcpy(d->groups, groups, bytes);
  d->group_count = (unsigned)count;
  return 0;
}
