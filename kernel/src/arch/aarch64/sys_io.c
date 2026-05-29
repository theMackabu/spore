#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mem.h"
#include "proc/io.h"

#include <stdint.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
  MAX_IOVCNT = 1024,
};

struct iovec64 {
  uint64_t base;
  uint64_t len;
};

static bool checked_add(uint64_t a, uint64_t b, uint64_t *out) {
  *out = a + b;
  return *out >= a;
}

int64_t sys_write(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len) {
  if (!syscall_user_readable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_write((int)fd, buf, len, frame);
}

int64_t sys_read(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len) {
  if (!syscall_user_writable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_read((int)fd, buf, len, frame);
}

int64_t sys_pread64(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t off) {
  if (!syscall_user_writable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_pread((int)fd, buf, len, off, frame);
}

int64_t sys_pwrite64(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t off) {
  if (!syscall_user_readable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_pwrite((int)fd, buf, len, off, frame);
}

int64_t sys_writev(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt) {
  if (iovcnt > MAX_IOVCNT) { return -(int64_t)EINVAL; }
  uint64_t iov_bytes;
  if (!checked_add(0, iovcnt * sizeof(struct iovec64), &iov_bytes) ||
      (iovcnt != 0 && iov_bytes / sizeof(struct iovec64) != iovcnt) || !syscall_user_readable(iov, iov_bytes)) {
    return -(int64_t)EFAULT;
  }

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
  if ((iovcnt != 0 && iov_bytes / sizeof(struct iovec64) != iovcnt) || !syscall_user_readable(iov, iov_bytes)) {
    return -(int64_t)EFAULT;
  }
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
