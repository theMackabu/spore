#include "abi/linux/shared/fd/handlers.h"
#include "abi/linux/shared/iovec.h"
#include "abi/linux/shared/numbers.h"
#include "abi/linux/shared/unix.h"
#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "net/socket.h"
#include "proc/domain.h"
#include "proc/eventfd.h"
#include "proc/io.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "proc/policy.h"

int uring_retry_io(struct domain *d, struct open_file *f, const struct sqe *e) {
  bool write = e->opcode == OP_WRITE || e->opcode == OP_SEND;
  if (!cell_domain_ensure_user_range(d, e->addr, e->len, write ? VMM_ACCESS_READ : VMM_ACCESS_WRITE)) {
    return -EFAULT;
  }
  if (f->type == OPEN_VFS) { return cell_file_regular_io(d, f, e->addr, e->len, e->off, write); }
  if (f->type == OPEN_EVENTFD) { return cell_eventfd_io(d, f, e->addr, e->len, write, NULL); }
  if (f->type == OPEN_PIPE) {
    return write ? cell_pipe_write_from_domain(d, f, e->addr, e->len) : cell_pipe_read_to_domain(d, f, e->addr, e->len);
  }
  uint32_t flags = e->opcode == OP_RECV || e->opcode == OP_SEND ? e->op_flags : 0;
  if (f->type == OPEN_UNIX_STREAM) {
    if (!f->unix_connected) { return -107; }
    return write ? cell_pipe_write_user(d, f->unix_tx_pipe, e->addr, e->len)
                 : cell_pipe_recv_user(d, f->unix_rx_pipe, e->addr, e->len, flags);
  }
  if (f->type == OPEN_SOCKET && f->socket_proto == 6) {
    return write ? cell_socket_tcp_write_from_domain(d, f, e->addr, e->len)
                 : cell_socket_tcp_read_to_domain_flags(d, f, e->addr, e->len, flags);
  }
  if (f->type == OPEN_SOCKET && !write) { return dgram_read_to_domain(d, f, e->addr, e->len, 0, 0, flags); }
  return -95;
}

static int single_io(struct open_file *ring, struct open_file *f, const struct sqe *e, bool may_defer) {
  if (e->len > INT32_MAX) { return -EINVAL; }
  int rc = uring_retry_io(cell_current_domain_internal(), f, e);
  if (rc == -95 && !(e->flags & FIXED_FILE)) {
    rc = e->opcode == OP_WRITE ? sys_write(NULL, e->fd, e->addr, e->len) : sys_read(NULL, e->fd, e->addr, e->len);
  }
  if (rc == -11 && may_defer && !(f->flags & 04000) && !(e->op_flags & 8)) { return uring_defer_io(ring, f, e); }
  return rc;
}

int uring_rw(struct open_file *ring, const struct sqe *e) {
  if (e->op_flags & ~8u) { return -95; }
  bool vector = e->opcode == OP_READV || e->opcode == OP_WRITEV;
  bool write = e->opcode == OP_WRITE || e->opcode == OP_WRITEV;
  if (e->off != UINT64_MAX && !cell_syscall_allowed(write ? SYS_PWRITE64 : SYS_PREAD64)) { return -1; }
  struct open_file *target = uring_target(&ring->ring, e);
  if (!target) { return -EBADF; }
  if (target->type == OPEN_URING) { return -EINVAL; }
  if (target->type != OPEN_VFS && e->off && e->off != UINT64_MAX) { return -29; }
  if ((e->flags & IO_ASYNC) || uring_ordered(&ring->ring, e)) {
    if (vector && (e->len > 1024 || !syscall_user_readable(e->addr, e->len * sizeof(struct iovec64)))) {
      return -EINVAL;
    }
    if (!e->len) { return 0; }
    return uring_defer_io(ring, target, e);
  }
  if (!vector) { return single_io(ring, target, e, true); }
  if (e->len > 1024) { return -EINVAL; }
  if (target->type == OPEN_UNIX_STREAM && cell_pipe_is_packet(target->unix_rx_pipe)) {
    int n = linux_unix_vector(cell_current_domain_internal(), target, e->addr, e->len, write, NULL);
    if (n == -11 && !(target->flags & 04000) && !(e->op_flags & 8)) { return uring_defer_io(ring, target, e); }
    return n;
  }
  if (target->type == OPEN_SOCKET && target->socket_proto != 6) {
    if (e->flags & FIXED_FILE) { return -95; }
    return sys_vector_io(NULL, e->fd, e->addr, e->len, write);
  }
  if (!syscall_user_readable(e->addr, e->len * sizeof(struct iovec64))) { return -EFAULT; }
  int total = 0;
  struct sqe part = *e;
  part.opcode = write ? OP_WRITE : OP_READ;
  for (unsigned i = 0; i < e->len; ++i) {
    struct iovec64 v;
    if (!vmm_copy_from_user(syscall_active_as(), &v, e->addr + i * sizeof(v), sizeof(v))) {
      return total ? total : -EFAULT;
    }
    if (v.len > INT32_MAX - (unsigned)total) { return total ? total : -EINVAL; }
    if (!v.len) { continue; }
    part.addr = v.base;
    part.len = v.len;
    int n = single_io(ring, target, &part, false);
    if (n == -11 && !total && !(target->flags & 04000) && !(e->op_flags & 8)) {
      return uring_defer_io(ring, target, e);
    }
    if (n < 0) { return total ? total : n; }
    total += n;
    if (n != (int)v.len) { break; }
    if (part.off != UINT64_MAX) { part.off += n; }
  }
  return total;
}

int uring_retry_vector(struct domain *d, struct open_file *target, const struct sqe *e, const struct iovec64 *iov) {
  if (target->type == OPEN_UNIX_STREAM && cell_pipe_is_packet(target->unix_rx_pipe)) {
    return linux_unix_vector(d, target, 0, e->len, e->opcode == OP_WRITEV, iov);
  }
  int total = 0;
  struct sqe part = *e;
  part.opcode = e->opcode == OP_WRITEV ? OP_WRITE : OP_READ;
  for (unsigned i = 0; i < e->len; ++i) {
    if (iov[i].len > INT32_MAX - (unsigned)total) { return total ? total : -EINVAL; }
    if (!iov[i].len) { continue; }
    part.addr = iov[i].base;
    part.len = iov[i].len;
    int n = uring_retry_io(d, target, &part);
    if (n < 0) { return total ? total : n; }
    total += n;
    if (n != (int)part.len) { break; }
    if (part.off != UINT64_MAX) { part.off += n; }
  }
  return total;
}
