#include "socket_msg.h"

#include "handlers.h"
#include "proc/fd.h"
#include "proc/signal.h"
#include "net/socket.h"
#include "proc/thread.h"

#include <stddef.h>

enum {
  EAGAIN = 11,
  MAX_SCM_RIGHTS = 8,
  MSG_CTRUNC = 0x8,
  MSG_CMSG_CLOEXEC = 0x40000000,
  SCM_RIGHTS = 1,
};

struct cmsghdr64 {
  uint64_t len;
  int32_t level;
  int32_t type;
};

struct unix_right_batch {
  int32_t fds[MAX_SCM_RIGHTS];
  size_t count;
};

static uint64_t cmsg_align(uint64_t len) {
  return (len + sizeof(uint64_t) - 1u) & ~(uint64_t)(sizeof(uint64_t) - 1u);
}

static int collect_unix_rights(const struct msghdr64 *msg, struct unix_right_batch *rights) {
  if (rights == NULL) { return -EINVAL; }
  rights->count = 0;
  if (msg == NULL || msg->control == 0 || msg->controllen == 0) { return 0; }
  if (!syscall_user_readable(msg->control, msg->controllen)) { return -EFAULT; }
  uint64_t off = 0;
  while (off + sizeof(struct cmsghdr64) <= msg->controllen) {
    struct cmsghdr64 cmsg;
    if (!vmm_copy_from_user(syscall_active_as(), &cmsg, msg->control + off, sizeof(cmsg))) { return -EFAULT; }
    if (cmsg.len < sizeof(struct cmsghdr64) || off + cmsg.len > msg->controllen) { return -EINVAL; }
    if (cmsg.level == SOL_SOCKET && cmsg.type == SCM_RIGHTS) {
      uint64_t data_off = off + sizeof(struct cmsghdr64);
      uint64_t data_len = cmsg.len - sizeof(struct cmsghdr64);
      uint64_t count = data_len / sizeof(int32_t);
      for (uint64_t i = 0; i < count; ++i) {
        if (rights->count >= MAX_SCM_RIGHTS) { return -EINVAL; }
        int32_t passed_fd = -1;
        if (!vmm_copy_from_user(syscall_active_as(), &passed_fd, msg->control + data_off + i * sizeof(passed_fd),
                                sizeof(passed_fd))) {
          return -EFAULT;
        }
        rights->fds[rights->count++] = passed_fd;
      }
    }
    uint64_t next = off + cmsg_align(cmsg.len);
    if (next <= off) { break; }
    off = next;
  }
  return 0;
}

static int queue_unix_rights(uint64_t fd, const struct unix_right_batch *rights, uint64_t start, uint64_t end) {
  if (rights == NULL) { return 0; }
  for (size_t i = 0; i < rights->count; ++i) {
    int rc = cell_fd_unix_queue_right_range((int)fd, rights->fds[i], start, end);
    if (rc < 0) { return rc; }
  }
  return 0;
}

static int finish_unix_control_msg(uint64_t msg_addr, uint32_t controllen, int32_t flags) {
  if (!syscall_user_writable(msg_addr + offsetof(struct msghdr64, controllen), sizeof(controllen)) ||
      !syscall_user_writable(msg_addr + offsetof(struct msghdr64, flags), sizeof(flags))) {
    return -EFAULT;
  }
  if (!vmm_copy_to_user(syscall_active_as(), msg_addr + offsetof(struct msghdr64, controllen), &controllen,
                        sizeof(controllen)) ||
      !vmm_copy_to_user(syscall_active_as(), msg_addr + offsetof(struct msghdr64, flags), &flags, sizeof(flags))) {
    return -EFAULT;
  }
  return 0;
}

static int copy_unix_right_to_msg(uint64_t fd, const struct msghdr64 *msg, uint64_t msg_addr, uint64_t flags,
                                  uint64_t start, uint64_t end) {
  uint64_t cmsg_len = sizeof(struct cmsghdr64) + sizeof(int32_t);
  uint64_t cmsg_space = cmsg_align(sizeof(struct cmsghdr64)) + cmsg_align(sizeof(int32_t));
  uint32_t out_controllen = 0;
  int32_t out_flags = 0;
  int fd_flags = (flags & MSG_CMSG_CLOEXEC) != 0 ? FD_CLOEXEC : 0;

  if (msg == NULL) { return finish_unix_control_msg(msg_addr, 0, 0); }
  int newfd = cell_fd_unix_recv_right_for_range((int)fd, start, end, fd_flags);
  if (newfd == -EAGAIN) { return finish_unix_control_msg(msg_addr, 0, 0); }
  if (newfd < 0) { return newfd; }
  if (msg->control == 0 || msg->controllen < cmsg_space) {
    (void)cell_fd_close(newfd);
    return finish_unix_control_msg(msg_addr, 0, MSG_CTRUNC);
  }
  if (!syscall_user_writable(msg->control, cmsg_space)) {
    (void)cell_fd_close(newfd);
    return -EFAULT;
  }
  struct cmsghdr64 cmsg = {
    .len = cmsg_len,
    .level = SOL_SOCKET,
    .type = SCM_RIGHTS,
  };
  int32_t fd32 = newfd;
  uint32_t zero = 0;
  if (!vmm_copy_to_user(syscall_active_as(), msg->control, &cmsg, sizeof(cmsg)) ||
      !vmm_copy_to_user(syscall_active_as(), msg->control + cmsg_align(sizeof(cmsg)), &fd32, sizeof(fd32)) ||
      (cmsg_space > cmsg_len &&
       !vmm_copy_to_user(syscall_active_as(), msg->control + cmsg_len, &zero, cmsg_space - cmsg_len))) {
    (void)cell_fd_close(newfd);
    return -EFAULT;
  }
  out_controllen = (uint32_t)cmsg_space;
  return finish_unix_control_msg(msg_addr, out_controllen, out_flags);
}

static int64_t send_iovecs_stream(struct trap_frame *frame, uint64_t fd, uint64_t iov_addr, int32_t iovlen,
                                  uint64_t flags) {
  if (iovlen == 0) { return 0; }
  int64_t total = 0;
  for (int32_t i = 0; i < iovlen; ++i) {
    struct iovec64 iov;
    int rc = aarch64_copy_socket_iov(iov_addr, iovlen, i, &iov);
    if (rc < 0) { return total == 0 ? (int64_t)rc : total; }
    if (iov.len == 0) { continue; }
    int64_t wrote = sys_sendto(total == 0 ? frame : NULL, fd, iov.base, iov.len, flags, 0, 0);
    if (wrote == CELL_SWITCHED) { return wrote; }
    if (wrote < 0) { return total == 0 ? wrote : total; }
    total += wrote;
    if ((uint64_t)wrote != iov.len) { break; }
  }
  return total;
}

int64_t aarch64_unix_recvmsg(struct trap_frame *frame, uint64_t fd, const struct msghdr64 *msg, uint64_t msg_addr,
                             uint64_t flags) {
  uint64_t iov_addr = msg->iov;
  int32_t iovlen = msg->iovlen;
  uint64_t recv_start = 0;
  uint64_t right_start = 0;
  uint64_t right_end = 0;
  bool have_right = false;
  bool stop_at_boundary = false;
  uint64_t max_total = UINT64_MAX;

  if (!cell_fd_unix_rx_offset((int)fd, &recv_start)) { return -(int64_t)EBADF; }
  have_right = cell_fd_unix_next_right_range((int)fd, &right_start, &right_end);
  if (have_right) {
    if (right_start >= recv_start && right_end > recv_start) {
      max_total = right_end - recv_start;
      stop_at_boundary = true;
    } else if (right_start < recv_start && right_end > recv_start) {
      max_total = right_end - recv_start;
      stop_at_boundary = true;
    }
  }

  if (iovlen == 0) {
    int control_rc = copy_unix_right_to_msg(fd, msg, msg_addr, flags, recv_start, recv_start);
    return control_rc < 0 ? (int64_t)control_rc : 0;
  }
  int64_t total = 0;
  for (int32_t i = 0; i < iovlen; ++i) {
    struct iovec64 iov;
    int rc = aarch64_copy_socket_iov(iov_addr, iovlen, i, &iov);
    if (rc < 0) { return total == 0 ? (int64_t)rc : total; }
    if (iov.len == 0) { continue; }
    uint64_t remaining = max_total == UINT64_MAX ? iov.len : max_total - (uint64_t)total;
    if (remaining == 0) { break; }
    uint64_t target = iov.len < remaining ? iov.len : remaining;
    int64_t got = sys_recvfrom(total == 0 ? frame : NULL, fd, iov.base, target, flags, 0, 0);
    if (got == CELL_SWITCHED) { return got; }
    if (got < 0) { return total == 0 ? got : total; }
    total += got;
    if ((uint64_t)got != target || (stop_at_boundary && (uint64_t)total >= max_total)) { break; }
  }
  if (total >= 0 && msg_addr != 0) {
    int control_rc = copy_unix_right_to_msg(fd, msg, msg_addr, flags, recv_start, recv_start + (uint64_t)total);
    if (control_rc < 0 && total == 0) { return control_rc; }
  }
  return total;
}

int64_t aarch64_unix_sendmsg(struct trap_frame *frame, uint64_t fd, const struct msghdr64 *msg, uint64_t flags) {
  struct unix_right_batch rights;
  int rights_rc = collect_unix_rights(msg, &rights);
  if (rights_rc < 0) { return (int64_t)rights_rc; }
  uint64_t start = 0;
  if (!cell_fd_unix_tx_offset((int)fd, &start)) { return -(int64_t)EBADF; }
  struct trap_frame *send_frame = (flags & MSG_DONTWAIT) != 0 ? NULL : frame;
  int64_t rc = send_iovecs_stream(send_frame, fd, msg->iov, msg->iovlen, flags);
  if (rc == -(int64_t)EPIPE && (flags & MSG_NOSIGNAL) == 0 && frame != NULL && cell_signal_current(SIGPIPE, frame)) {
    return CELL_SWITCHED;
  }
  if (rc > 0) {
    rights_rc = queue_unix_rights(fd, &rights, start, start + (uint64_t)rc);
    if (rights_rc < 0) { return (int64_t)rights_rc; }
  }
  return rc;
}
