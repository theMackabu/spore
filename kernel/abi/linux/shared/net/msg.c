#include "proc/fd.h"
#include "handlers.h"
#include "message_abi.h"
#include "abi/linux/shared/net/handlers.h"
#include "abi/linux/shared/usercopy.h"
#include "net/socket.h"
#include "net/route.h"
#include "net/unix.h"
#include "proc/thread.h"
#include "proc/signal.h"

enum {
  EMSGSIZE = 90,
  MAX_IOVCNT = 1024,
  SENDMSG_SCRATCH_CAP = 1472,
};

int linux_copy_socket_iov(uint64_t iov_addr, int32_t iovlen, int32_t index, struct iovec64 *out) {
  if (iovlen < 0 || iovlen > MAX_IOVCNT || index < 0 || index >= iovlen || out == NULL) { return -EINVAL; }
  if (iov_addr == 0 || !syscall_user_readable(iov_addr + (uint64_t)index * sizeof(*out), sizeof(*out))) {
    return -EFAULT;
  }
  return vmm_copy_from_user(syscall_active_as(), out, iov_addr + (uint64_t)index * sizeof(*out), sizeof(*out))
           ? 0
           : -EFAULT;
}

static int gather_iovecs(uint64_t iov_addr, int32_t iovlen, uint8_t *dst, size_t cap, uint64_t *total_len,
                         size_t *copied) {
  if (iovlen < 0 || iovlen > MAX_IOVCNT || dst == NULL || total_len == NULL || copied == NULL) { return -EINVAL; }
  if (iovlen > 0 && iov_addr == 0) { return -EFAULT; }
  *total_len = 0;
  *copied = 0;
  for (int32_t i = 0; i < iovlen; ++i) {
    struct iovec64 iov;
    int rc = linux_copy_socket_iov(iov_addr, iovlen, i, &iov);
    if (rc < 0) { return rc; }
    if (iov.len != 0 && !syscall_user_readable(iov.base, iov.len)) { return -EFAULT; }
    if (UINT64_MAX - *total_len < iov.len) { return -EINVAL; }
    *total_len += iov.len;
    if (*copied >= cap || iov.len == 0) { continue; }
    uint64_t room = cap - *copied;
    size_t n = (size_t)(iov.len < room ? iov.len : room);
    if (!vmm_copy_from_user(syscall_active_as(), dst + *copied, iov.base, n)) { return -EFAULT; }
    *copied += n;
  }
  return 0;
}

int64_t sys_sendmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags) {
  if (msg_addr == 0 || !syscall_user_readable(msg_addr, sizeof(struct msghdr64))) { return -(int64_t)EFAULT; }
  struct msghdr64 msg;
  if (!vmm_copy_from_user(syscall_active_as(), &msg, msg_addr, sizeof(msg))) { return -(int64_t)EFAULT; }
  int32_t proto = 0;
  bool is_socket = cell_fd_socket_info((int)fd, NULL, &proto);
  if (is_socket && proto == IPPROTO_TCP) {
    int64_t rc = cell_fd_socket_sendmsg((int)fd, msg_addr, (flags & MSG_DONTWAIT) != 0, frame);
    if (rc == -(int64_t)EPIPE && (flags & MSG_NOSIGNAL) == 0 && frame != NULL && cell_signal_current(SIGPIPE, frame)) {
      return CELL_SWITCHED;
    }
    return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
  }
  struct open_file *file = cell_fd_file(fd);
  if (is_socket && file->type != OPEN_SOCKET && proto == 0 && msg.name == 0) { return linux_unix_message(frame, (int)fd, msg_addr, &msg, (uint32_t)flags, true); }
  uint8_t tmp[SENDMSG_SCRATCH_CAP];
  uint64_t total_len = 0;
  size_t copied = 0;
  int rc = gather_iovecs(msg.iov, msg.iovlen, tmp, sizeof(tmp), &total_len, &copied);
  if (rc < 0) { return (int64_t)rc; }

  if (file && file->type == OPEN_SOCKET) {
    if (msg.controllen && (file->socket_proto == UNIX_DGRAM_PROTOCOL || file->socket_proto == ROUTE_PROTOCOL)) { return -95; }
    if (total_len > sizeof(tmp)) { return -EMSGSIZE; }
    return linux_send_datagram(fd, tmp, total_len, msg.name, msg.namelen);
  }
  if (msg.iovlen == 0) { return 0; }
  struct iovec64 first;
  rc = linux_copy_socket_iov(msg.iov, msg.iovlen, 0, &first);
  if (rc < 0) { return (int64_t)rc; }
  return sys_sendto(frame, fd, first.base, first.len, flags, msg.name, msg.namelen);
}

int64_t sys_recvmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags) {
  if (msg_addr == 0 || !syscall_user_readable(msg_addr, sizeof(struct msghdr64)) ||
      !syscall_user_writable(msg_addr, sizeof(struct msghdr64))) {
    return -(int64_t)EFAULT;
  }
  int32_t proto = 0;
  struct open_file *file = cell_fd_file(fd);
  if (file && file->type == OPEN_SOCKET) {
    int64_t rc = cell_fd_socket_recvmsg((int)fd, msg_addr, (uint32_t)flags, (flags & MSG_DONTWAIT) != 0, frame);
    return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
  }
  struct msghdr64 msg;
  if (!vmm_copy_from_user(syscall_active_as(), &msg, msg_addr, sizeof(msg))) { return -(int64_t)EFAULT; }
  if (cell_fd_socket_info((int)fd, NULL, &proto) && proto == 0) {
    int64_t rc = linux_unix_message(frame, (int)fd, msg_addr, &msg, (uint32_t)flags, false);
    return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
  }
  struct iovec64 iov;
  int rc_iov = linux_copy_socket_iov(msg.iov, msg.iovlen, 0, &iov);
  if (rc_iov < 0) { return (int64_t)rc_iov; }
  int64_t rc = sys_recvfrom(frame, fd, iov.base, iov.len, flags, msg.name, msg_addr + 8);
  if (rc >= 0) {
    msg.flags = 0;
    (void)vmm_copy_to_user(syscall_active_as(), msg_addr, &msg, sizeof(msg));
  }
  return rc;
}
