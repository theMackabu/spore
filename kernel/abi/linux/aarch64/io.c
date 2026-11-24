#include "message_layout.h"
#include "arch/aarch64/regs.h"
#include "handlers.h"
#include "fs/notify.h"
#include "proc/fd.h"
#include "proc/io.h"
#include "proc/limits.h"
#include "net/socket.h"
#include "proc/thread.h"
#include "fs/vfs.h"

#include <stdint.h>

enum {
  AT_FDCWD = -100,
  EFAULT = 14,
  EINVAL = 22,
  ENOENT = 2,
  MAX_IOVCNT = 1024,
};

int64_t sys_write(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len) {
  if (!syscall_user_readable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_write((int)fd, buf, len, frame);
}

int64_t sys_read(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len) {
  if (!syscall_user_writable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_read((int)fd, buf, len, frame);
}

int64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t off) {
  if (!syscall_user_writable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_positioned_io((int)fd, buf, len, (int64_t)off, false);
}

int64_t sys_pwrite64(uint64_t fd, uint64_t buf, uint64_t len, uint64_t off) {
  if (!syscall_user_readable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_positioned_io((int)fd, buf, len, (int64_t)off, true);
}

int64_t sys_writev(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt) {
  if (iovcnt > MAX_IOVCNT) { return -(int64_t)EINVAL; }
  uint64_t iov_bytes = iovcnt * sizeof(struct iovec64);
  if (!syscall_user_readable(iov, iov_bytes)) {
    return -(int64_t)EFAULT;
  }

  if (cell_fd_is_datagram((int)fd)) { return cell_fd_dgram_writev((int)fd, iov, (int32_t)iovcnt); }
  int64_t total = 0;
  for (uint64_t i = 0; i < iovcnt; ++i) {
    struct iovec64 v;
    if (!vmm_copy_from_user(syscall_active_as(), &v, iov + i * sizeof(v), sizeof(v))) { return -(int64_t)EFAULT; }
    int64_t wrote = sys_write(total == 0 ? frame : NULL, fd, v.base, v.len);
    if (wrote == CELL_SWITCHED) { return wrote; }
    if (wrote < 0) { return total == 0 ? wrote : total; }
    total += wrote;
    if ((uint64_t)wrote != v.len) { break; }
  }
  return total;
}

int64_t sys_readv(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt) {
  if (iovcnt > MAX_IOVCNT) { return -(int64_t)EINVAL; }
  uint64_t iov_bytes = iovcnt * sizeof(struct iovec64);
  if (!syscall_user_readable(iov, iov_bytes)) {
    return -(int64_t)EFAULT;
  }
  if (cell_fd_is_datagram((int)fd)) { return cell_fd_dgram_readv((int)fd, iov, (int32_t)iovcnt, frame); }
  int64_t total = 0;
  for (uint64_t i = 0; i < iovcnt; ++i) {
    struct iovec64 v;
    if (!vmm_copy_from_user(syscall_active_as(), &v, iov + i * sizeof(v), sizeof(v))) { return -(int64_t)EFAULT; }
    int64_t got = sys_read(total == 0 ? frame : NULL, fd, v.base, v.len);
    if (got == CELL_SWITCHED) { return got; }
    if (got < 0) { return total == 0 ? got : total; }
    total += got;
    if ((uint64_t)got != v.len) { break; }
  }
  return total;
}

int64_t sys_inotify_init1(uint64_t flags) {
  return inotify_open((int)flags);
}

int64_t sys_inotify_add_watch(uint64_t fd, uint64_t path_addr, uint64_t mask) {
  char path[CELL_PATH_MAX];
  int64_t rc = syscall_copy_resolved_path_at(AT_FDCWD, path_addr, path, sizeof(path));
  if (rc) { return rc; }
  struct vfs_node node;
  bool exists = mask & FS_DONT_FOLLOW ? vfs_lstat(path, &node) : vfs_lookup(path, &node);
  if (!exists) { return -ENOENT; }
  if (!syscall_node_access_allowed(&node, 4)) { return -13; }
  return inotify_add((int)fd, &node, (uint32_t)mask);
}

int64_t sys_inotify_rm_watch(uint64_t fd, uint64_t wd) {
  return inotify_remove((int)fd, (int)wd);
}
