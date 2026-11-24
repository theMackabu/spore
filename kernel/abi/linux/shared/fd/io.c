#include "proc/io.h"
#include "../iovec.h"
#include "../usercopy.h"
#include "abi/linux/shared/unix.h"
#include "handlers.h"
#include "net/socket.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/pipe.h"
#include "proc/thread.h"

#include <stdint.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
  MAX_IOVCNT = 1024,
};

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

int64_t sys_vector_io(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt, bool write) {
  if (iovcnt > MAX_IOVCNT) { return -(int64_t)EINVAL; }
  uint64_t iov_bytes = iovcnt * sizeof(struct iovec64);
  if (!syscall_user_readable(iov, iov_bytes)) {
    return -(int64_t)EFAULT;
  }

  struct open_file *file = cell_fd_file(fd);
  if (file && file->type == OPEN_UNIX_STREAM && cell_pipe_is_packet(file->unix_rx_pipe)) {
    int64_t n = linux_unix_vector(cell_current_domain_internal(), file, iov, iovcnt, write, NULL);
    if (n != -11 || !frame || (file->flags & 04000)) { return n; }
    return cell_block_current(frame, (struct thread_wait){.reason = WAIT_PIPE,
                                                          .target = fd,
                                                          .socket_iov = iov,
                                                          .socket_iovlen = iovcnt,
                                                          .write = write,
                                                          .socket_vector = true});
  }
  if (cell_fd_is_datagram((int)fd)) {
    return write ? cell_fd_dgram_writev((int)fd, iov, (int32_t)iovcnt)
                 : cell_fd_dgram_readv((int)fd, iov, (int32_t)iovcnt, frame);
  }
  int64_t total = 0;
  for (uint64_t i = 0; i < iovcnt; ++i) {
    struct iovec64 v;
    if (!vmm_copy_from_user(syscall_active_as(), &v, iov + i * sizeof(v), sizeof(v))) { return -(int64_t)EFAULT; }
    struct trap_frame *wait = total == 0 ? frame : NULL;
    int64_t count = write ? sys_write(wait, fd, v.base, v.len) : sys_read(wait, fd, v.base, v.len);
    if (count == CELL_SWITCHED) { return count; }
    if (count < 0) { return total == 0 ? count : total; }
    total += count;
    if ((uint64_t)count != v.len) { break; }
  }
  return total;
}
