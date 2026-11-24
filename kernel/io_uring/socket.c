#include "net/socket.h"
#include "abi/linux/shared/message.h"
#include "abi/linux/shared/net/handlers.h"
#include "internal.h"
#include "proc/domain.h"
#include "proc/thread.h"

int uring_retry_socket(struct domain *d, struct open_file *f, const struct sqe *e) {
  struct thread_wait wait = {0};
  switch (e->opcode) {
  case OP_CONNECT:
    break;
  case OP_ACCEPT:
    wait.socket_accept_flags = e->op_flags;
    wait.socket_addr = e->addr;
    wait.socket_addrlen = e->off;
    break;
  case OP_SENDMSG:
    wait.write = true;
    wait.socket_msg = true;
    wait.socket_msg_addr = e->addr;
    break;
  case OP_RECVMSG: {
    struct socket_user_message m;
    int rc = linux_read_socket_message(d, e->addr, &m);
    if (rc) { return rc; }
    wait.socket_msg = true;
    wait.socket_msg_addr = e->addr;
    wait.socket_iov = m.iov;
    wait.socket_iovlen = m.iovlen;
    wait.socket_addr = m.name;
    wait.socket_addrlen = m.namelen_addr;
    wait.socket_flags = e->op_flags;
    break;
  }
  default:
    return -EINVAL;
  }
  return cell_resume_socket_io(f, d, &wait);
}

int uring_socket_op(struct open_file *ring, const struct sqe *e) {
  struct open_file *f = cell_fd_file(e->fd);
  if (!f) { return -EBADF; }
  if (f->type != OPEN_SOCKET && f->type != OPEN_UNIX_STREAM && f->type != OPEN_UNIX_LISTENER) { return -88; }
  int rc;
  bool nowait = false;
  switch (e->opcode) {
  case OP_ACCEPT:
    if (e->priority == 1) {
      bool accepting = false;
      if (!cell_fd_socket_accepting(e->fd, &accepting) || !accepting || (e->op_flags & ~(04000u | 02000000u))) {
        return -EINVAL;
      }
      return uring_defer_io(ring, f, e);
    }
    rc = sys_accept4(NULL, e->fd, e->addr, e->off, e->op_flags);
    break;
  case OP_CONNECT:
    rc = sys_connect(NULL, e->fd, e->addr, e->off);
    if (rc != -115) { return rc; }
    return uring_defer_io(ring, f, e);
  case OP_SEND:
    rc = sys_sendto(NULL, e->fd, e->addr, e->len, e->op_flags, e->off, e->file_index);
    break;
  case OP_RECV:
    rc = sys_recvfrom(NULL, e->fd, e->addr, e->len, e->op_flags, 0, 0);
    break;
  case OP_SENDMSG:
    rc = sys_sendmsg(NULL, e->fd, e->addr, e->op_flags);
    break;
  case OP_RECVMSG:
    rc = sys_recvmsg(NULL, e->fd, e->addr, e->op_flags);
    break;
  default:
    return -EINVAL;
  }
  if (e->opcode != OP_ACCEPT) { nowait = e->op_flags & 0x40; }
  if (rc != -11 || nowait) { return rc; }
  if ((e->opcode == OP_RECVMSG || e->opcode == OP_SENDMSG) && f->type != OPEN_SOCKET) { return -95; }
  if (e->opcode == OP_SEND && f->type == OPEN_SOCKET && f->socket_proto != 6) { return rc; }
  return uring_defer_io(ring, f, e);
}
