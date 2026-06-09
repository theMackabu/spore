#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mem.h"
#include "proc/io.h"
#include "proc/socket.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  AF_UNSPEC = 0,
  AF_UNIX = 1,
  AF_INET = 2,
  SOCK_STREAM = 1,
  SOCK_DGRAM = 2,
  SOL_SOCKET = 1,
  IPPROTO_IP = 0,
  SO_TYPE = 3,
  SO_REUSEADDR = 2,
  SO_ERROR = 4,
  SO_DONTROUTE = 5,
  SO_BROADCAST = 6,
  SO_SNDBUF = 7,
  SO_RCVBUF = 8,
  SO_KEEPALIVE = 9,
  SO_LINGER = 13,
  SO_REUSEPORT = 15,
  SO_PEERCRED = 17,
  SO_RCVLOWAT = 18,
  SO_SNDLOWAT = 19,
  SO_RCVTIMEO_OLD = 20,
  SO_SNDTIMEO_OLD = 21,
  SO_ACCEPTCONN = 30,
  SO_PROTOCOL = 38,
  SO_DOMAIN = 39,
  SO_RCVTIMEO_NEW = 66,
  SO_SNDTIMEO_NEW = 67,
  IP_TOS = 1,
  IP_TTL = 2,
  IP_MTU_DISCOVER = 10,
  IP_BIND_ADDRESS_NO_PORT = 24,
  IPPROTO_ICMP = 1,
  IPPROTO_TCP = 6,
  IPPROTO_UDP = 17,
  TCP_NODELAY = 1,
  TCP_KEEPIDLE = 4,
  TCP_KEEPINTVL = 5,
  TCP_KEEPCNT = 6,
  TCP_USER_TIMEOUT = 18,
  TCP_FASTOPEN = 23,
  TCP_FASTOPEN_CONNECT = 30,
  EPERM = 1,
  EBADF = 9,
  EFAULT = 14,
  EINVAL = 22,
  EAGAIN = 11,
  EMSGSIZE = 90,
  EAFNOSUPPORT = 97,
  ENOPROTOOPT = 92,
  EPROTONOSUPPORT = 93,
  ENOTCONN = 107,
  EISCONN = 106,
  EPIPE = 32,
  SIGPIPE = 13,
  MAX_IOVCNT = 1024,
  MAX_SCM_RIGHTS = 8,
  SENDMSG_SCRATCH_CAP = 1472,
  F_GETFD = 1,
  FD_CLOEXEC = 1,
  O_NONBLOCK = 04000,
  O_CLOEXEC = 02000000,
  MSG_PEEK = 0x2,
  MSG_DONTWAIT = 0x40,
  MSG_NOSIGNAL = 0x4000,
  MSG_TRUNC = 0x20,
  MSG_WAITALL = 0x100,
  MSG_CTRUNC = 0x8,
  MSG_CMSG_CLOEXEC = 0x40000000,
  SCM_RIGHTS = 1,
};

struct iovec64 {
  uint64_t base;
  uint64_t len;
};

struct msghdr64 {
  uint64_t name;
  uint32_t namelen;
  uint32_t pad1;
  uint64_t iov;
  int32_t iovlen;
  int32_t pad2;
  uint64_t control;
  uint32_t controllen;
  uint32_t pad3;
  int32_t flags;
  int32_t pad4;
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

struct sockaddr_in64 {
  uint16_t sin_family;
  uint16_t sin_port;
  uint32_t sin_addr;
  uint8_t sin_zero[8];
};

struct sockaddr_un64 {
  uint16_t sun_family;
  char sun_path[108];
};

struct ucred64 {
  int32_t pid;
  uint32_t uid;
  uint32_t gid;
};

struct linger64 {
  int32_t onoff;
  int32_t linger;
};

struct timeval64 {
  int64_t sec;
  int64_t usec;
};

static uint16_t bswap16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

static bool copy_sockaddr_in(uint64_t addr, uint64_t len, struct sockaddr_in64 *out) {
  if (addr == 0 || len < sizeof(*out) || !syscall_user_readable(addr, sizeof(*out))) { return false; }
  return vmm_copy_from_user(syscall_active_as(), out, addr, sizeof(*out)) && out->sin_family == AF_INET;
}

static int socket_copy_timeout_to_user(uint64_t optval, uint64_t optlen_addr, uint64_t ticks);

static bool copy_sockaddr_family(uint64_t addr, uint64_t len, uint16_t *family) {
  if (addr == 0 || len < sizeof(uint16_t) || !syscall_user_readable(addr, sizeof(uint16_t))) { return false; }
  return vmm_copy_from_user(syscall_active_as(), family, addr, sizeof(*family));
}

static bool copy_sockaddr_un(uint64_t addr, uint64_t len, struct sockaddr_un64 *out) {
  if (addr == 0 || len < 3 || len > sizeof(*out) || !syscall_user_readable(addr, len)) { return false; }
  kmemset(out, 0, sizeof(*out));
  if (!vmm_copy_from_user(syscall_active_as(), out, addr, (size_t)len) || out->sun_family != AF_UNIX) { return false; }
  out->sun_path[sizeof(out->sun_path) - 1] = '\0';
  return out->sun_path[0] == '/';
}

static int finish_socket_fd(int fd, uint64_t type) {
  if (fd < 0) { return fd; }
  if ((type & O_NONBLOCK) != 0) {
    int rc = cell_fd_set_flags(fd, O_NONBLOCK);
    if (rc < 0) { return rc; }
  }
  if ((type & O_CLOEXEC) != 0) {
    int rc = cell_fd_set_fd_flags(fd, FD_CLOEXEC);
    if (rc < 0) { return rc; }
  }
  return fd;
}

int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
  uint64_t base_type = type & 0xf;
  if ((type & ~(uint64_t)(0xf | O_NONBLOCK | O_CLOEXEC)) != 0) { return -(int64_t)EINVAL; }
  if (domain == AF_UNIX) {
    if (base_type != SOCK_STREAM || protocol != 0) { return -(int64_t)EPROTONOSUPPORT; }
    return finish_socket_fd(cell_fd_socket_unix(), type);
  }
  if (domain != AF_INET) { return -(int64_t)EAFNOSUPPORT; }
  if (base_type == SOCK_STREAM) {
    if (protocol == 0) { protocol = IPPROTO_TCP; }
    if (protocol != IPPROTO_TCP) { return -(int64_t)EPROTONOSUPPORT; }
    return finish_socket_fd(cell_fd_socket_inet((uint8_t)protocol), type);
  }
  if (base_type != SOCK_DGRAM) { return -(int64_t)EPROTONOSUPPORT; }
  if (protocol == 0) { protocol = IPPROTO_UDP; }
  if (protocol != IPPROTO_UDP && protocol != IPPROTO_ICMP) { return -(int64_t)EPROTONOSUPPORT; }
  return finish_socket_fd(cell_fd_socket_inet((uint8_t)protocol), type);
}

int64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t sv) {
  uint64_t base_type = type & 0xf;
  int flags = (int)(type & (O_NONBLOCK | O_CLOEXEC));
  if ((type & ~(uint64_t)(0xf | O_NONBLOCK | O_CLOEXEC)) != 0) { return -(int64_t)EINVAL; }
  if (domain != AF_UNIX) { return -(int64_t)EAFNOSUPPORT; }
  if (base_type != SOCK_STREAM || protocol != 0) { return -(int64_t)EPROTONOSUPPORT; }
  if (sv == 0 || !syscall_user_writable(sv, sizeof(int32_t) * 2)) { return -(int64_t)EFAULT; }
  return cell_fd_socketpair_unix(sv, flags);
}

int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len) {
  uint16_t family = 0;
  if (!copy_sockaddr_family(addr, len, &family)) { return -(int64_t)EINVAL; }
  if (family == AF_UNIX) {
    struct sockaddr_un64 sa_un;
    if (!copy_sockaddr_un(addr, len, &sa_un)) { return -(int64_t)EINVAL; }
    int rc = cell_fd_unix_bind((int)fd, sa_un.sun_path);
    return rc < 0 ? (int64_t)rc : 0;
  }
  struct sockaddr_in64 sa;
  if (!copy_sockaddr_in(addr, len, &sa)) { return -(int64_t)EINVAL; }
  int rc = cell_fd_tcp_bind((int)fd, bswap16(sa.sin_port));
  if (rc == -EBADF) { rc = cell_fd_udp_bind((int)fd, bswap16(sa.sin_port)); }
  return rc < 0 ? (int64_t)rc : 0;
}

int64_t sys_connect(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t len) {
  uint16_t family = 0;
  if (!copy_sockaddr_family(addr, len, &family)) { return -(int64_t)EINVAL; }
  if (family == AF_UNSPEC) {
    int rc = cell_fd_udp_disconnect((int)fd);
    return rc < 0 ? (int64_t)rc : 0;
  }
  if (family == AF_UNIX) {
    struct sockaddr_un64 sa_un;
    if (!copy_sockaddr_un(addr, len, &sa_un)) { return -(int64_t)EINVAL; }
    int rc = cell_fd_unix_connect((int)fd, sa_un.sun_path);
    return rc < 0 ? (int64_t)rc : 0;
  }
  struct sockaddr_in64 sa;
  if (!copy_sockaddr_in(addr, len, &sa)) { return -(int64_t)EINVAL; }
  int rc = cell_fd_tcp_connect((int)fd, sa.sin_addr, bswap16(sa.sin_port), frame);
  if (rc == CELL_SWITCHED) { return CELL_SWITCHED; }
  if (rc != -9) { return (int64_t)rc; }
  if (!cell_egress_allowed(IPPROTO_UDP, sa.sin_addr, bswap16(sa.sin_port))) { return -(int64_t)EPERM; }
  return cell_fd_udp_connect((int)fd, sa.sin_addr, bswap16(sa.sin_port)) ? 0 : -(int64_t)EBADF;
}

int64_t sys_listen(uint64_t fd, uint64_t backlog) {
  int rc = cell_fd_tcp_listen((int)fd, (int)backlog);
  if (rc == -EBADF) { rc = cell_fd_unix_listen((int)fd, (int)backlog); }
  return rc < 0 ? (int64_t)rc : 0;
}

int64_t sys_accept(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t addrlen) {
  int rc = cell_fd_tcp_accept((int)fd, addr, addrlen, 0, frame);
  if (rc == -EBADF) { rc = cell_fd_unix_accept((int)fd, 0, frame); }
  return rc == CELL_SWITCHED ? CELL_SWITCHED : (int64_t)rc;
}

int64_t sys_accept4(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t addrlen, uint64_t flags) {
  if ((flags & ~(uint64_t)(O_NONBLOCK | O_CLOEXEC)) != 0) { return -(int64_t)EINVAL; }
  int rc = cell_fd_tcp_accept((int)fd, addr, addrlen, (int)flags, frame);
  if (rc == -EBADF) { rc = cell_fd_unix_accept((int)fd, (int)flags, frame); }
  return rc == CELL_SWITCHED ? CELL_SWITCHED : (int64_t)rc;
}

int64_t sys_sendto(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                   uint64_t addrlen) {
  if (!syscall_user_readable(buf, len)) { return -(int64_t)EFAULT; }
  if (addr == 0) {
    int64_t tcp = cell_fd_tcp_send((int)fd, buf, len, (flags & MSG_DONTWAIT) != 0, frame);
    if (tcp == -(int64_t)EPIPE && (flags & MSG_NOSIGNAL) == 0 && frame != NULL && cell_signal_current(SIGPIPE, frame)) {
      return CELL_SWITCHED;
    }
    if (tcp != -EBADF && tcp != -9) { return tcp; }
    int64_t udp = cell_fd_udp_send((int)fd, 0, 0, buf, len);
    if (udp != -EBADF) { return udp; }
    int64_t rc = cell_fd_write((int)fd, buf, len, frame);
    return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
  }
  uint32_t ip = 0;
  uint16_t port = 0;
  struct sockaddr_in64 sa;
  if (!copy_sockaddr_in(addr, addrlen, &sa)) { return -(int64_t)EINVAL; }
  ip = sa.sin_addr;
  port = bswap16(sa.sin_port);
  return cell_fd_udp_send((int)fd, ip, port, buf, len);
}

int64_t sys_recvfrom(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                     uint64_t addrlen) {
  if (!syscall_user_writable(buf, len)) { return -(int64_t)EFAULT; }
  int64_t rc =
    cell_fd_socket_recv((int)fd, buf, len, (uint32_t)flags, (flags & MSG_DONTWAIT) != 0, frame, addr, addrlen);
  return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
}

static int copy_iov_at(uint64_t iov_addr, int32_t iovlen, int32_t index, struct iovec64 *out) {
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
    int rc = copy_iov_at(iov_addr, iovlen, i, &iov);
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
    int rc = copy_iov_at(iov_addr, iovlen, i, &iov);
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

static int64_t recv_iovecs_stream(struct trap_frame *frame, uint64_t fd, const struct msghdr64 *msg,
                                  uint64_t msg_addr, uint64_t flags) {
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
    int rc = copy_iov_at(iov_addr, iovlen, i, &iov);
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
  if (is_socket && proto == 0 && msg.name == 0) {
    struct unix_right_batch rights;
    int rights_rc = collect_unix_rights(&msg, &rights);
    if (rights_rc < 0) { return (int64_t)rights_rc; }
    uint64_t start = 0;
    if (!cell_fd_unix_tx_offset((int)fd, &start)) { return -(int64_t)EBADF; }
    struct trap_frame *send_frame = (flags & MSG_DONTWAIT) != 0 ? NULL : frame;
    int64_t rc = send_iovecs_stream(send_frame, fd, msg.iov, msg.iovlen, flags);
    if (rc == -(int64_t)EPIPE && (flags & MSG_NOSIGNAL) == 0 && frame != NULL && cell_signal_current(SIGPIPE, frame)) {
      return CELL_SWITCHED;
    }
    if (rc > 0) {
      rights_rc = queue_unix_rights(fd, &rights, start, start + (uint64_t)rc);
      if (rights_rc < 0) { return (int64_t)rights_rc; }
    }
    return rc;
  }
  uint8_t tmp[SENDMSG_SCRATCH_CAP];
  uint64_t total_len = 0;
  size_t copied = 0;
  int rc = gather_iovecs(msg.iov, msg.iovlen, tmp, sizeof(tmp), &total_len, &copied);
  if (rc < 0) { return (int64_t)rc; }

  if (msg.name != 0) {
    if (total_len > sizeof(tmp)) { return -(int64_t)EMSGSIZE; }
    struct sockaddr_in64 sa;
    if (!copy_sockaddr_in(msg.name, msg.namelen, &sa)) { return -(int64_t)EINVAL; }
    return cell_fd_udp_send_kernel((int)fd, sa.sin_addr, bswap16(sa.sin_port), tmp, total_len);
  }
  if (is_socket && (proto == IPPROTO_UDP || proto == IPPROTO_ICMP)) {
    if (total_len > sizeof(tmp)) { return -(int64_t)EMSGSIZE; }
    return cell_fd_udp_send_kernel((int)fd, 0, 0, tmp, total_len);
  }

  if (msg.iovlen == 0) { return 0; }
  struct iovec64 first;
  rc = copy_iov_at(msg.iov, msg.iovlen, 0, &first);
  if (rc < 0) { return (int64_t)rc; }
  return sys_sendto(frame, fd, first.base, first.len, flags, msg.name, msg.namelen);
}

int64_t sys_recvmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags) {
  if (msg_addr == 0 || !syscall_user_readable(msg_addr, sizeof(struct msghdr64)) ||
      !syscall_user_writable(msg_addr, sizeof(struct msghdr64))) {
    return -(int64_t)EFAULT;
  }
  int32_t proto = 0;
  if (cell_fd_socket_info((int)fd, NULL, &proto) &&
      (proto == IPPROTO_TCP || proto == IPPROTO_UDP || proto == IPPROTO_ICMP)) {
    int64_t rc = cell_fd_socket_recvmsg((int)fd, msg_addr, (uint32_t)flags, (flags & MSG_DONTWAIT) != 0, frame);
    return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
  }
  struct msghdr64 msg;
  if (!vmm_copy_from_user(syscall_active_as(), &msg, msg_addr, sizeof(msg))) { return -(int64_t)EFAULT; }
  if (cell_fd_socket_info((int)fd, NULL, &proto) && proto == 0) {
    int64_t rc = recv_iovecs_stream(frame, fd, &msg, msg_addr, flags);
    return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
  }
  struct iovec64 iov;
  int rc_iov = copy_iov_at(msg.iov, msg.iovlen, 0, &iov);
  if (rc_iov < 0) { return (int64_t)rc_iov; }
  int64_t rc = sys_recvfrom(frame, fd, iov.base, iov.len, flags, msg.name, msg_addr + 8);
  if (rc >= 0) {
    msg.flags = 0;
    (void)vmm_copy_to_user(syscall_active_as(), msg_addr, &msg, sizeof(msg));
  }
  return rc;
}

int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen) {
  if (addr == 0 || addrlen == 0 || !syscall_user_writable(addrlen, sizeof(uint32_t))) { return -(int64_t)EFAULT; }
  uint32_t len = sizeof(struct sockaddr_in64);
  (void)vmm_copy_to_user(syscall_active_as(), addrlen, &len, sizeof(len));
  if (!syscall_user_writable(addr, sizeof(struct sockaddr_in64))) { return -(int64_t)EFAULT; }
  uint32_t ip = 0;
  uint16_t port = 0;
  if (!cell_fd_socket_local_addr((int)fd, &ip, &port)) { return -(int64_t)EBADF; }
  struct sockaddr_in64 sa = {
    .sin_family = AF_INET,
    .sin_port = bswap16(port),
    .sin_addr = ip,
  };
  return vmm_copy_to_user(syscall_active_as(), addr, &sa, sizeof(sa)) ? 0 : -(int64_t)EFAULT;
}

int64_t sys_getpeername(uint64_t fd, uint64_t addr, uint64_t addrlen) {
  if (addr == 0 || addrlen == 0 || !syscall_user_writable(addrlen, sizeof(uint32_t))) { return -(int64_t)EFAULT; }
  uint32_t len = sizeof(struct sockaddr_in64);
  (void)vmm_copy_to_user(syscall_active_as(), addrlen, &len, sizeof(len));
  if (!syscall_user_writable(addr, sizeof(struct sockaddr_in64))) { return -(int64_t)EFAULT; }
  uint32_t ip = 0;
  uint16_t port = 0;
  if (!cell_fd_socket_peer_addr((int)fd, &ip, &port)) { return -(int64_t)ENOTCONN; }
  struct sockaddr_in64 sa = {
    .sin_family = AF_INET,
    .sin_port = bswap16(port),
    .sin_addr = ip,
  };
  return vmm_copy_to_user(syscall_active_as(), addr, &sa, sizeof(sa)) ? 0 : -(int64_t)EFAULT;
}

int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen_addr) {
  if (optval == 0 || optlen_addr == 0 || !syscall_user_writable(optlen_addr, sizeof(uint32_t))) {
    return -(int64_t)EINVAL;
  }

  uint32_t optlen = 0;
  if (!vmm_copy_from_user(syscall_active_as(), &optlen, optlen_addr, sizeof(optlen))) { return -(int64_t)EFAULT; }

  if (level == SOL_SOCKET && optname == SO_PEERCRED) {
    if (optlen < sizeof(struct ucred64) || !syscall_user_writable(optval, sizeof(struct ucred64))) {
      return -(int64_t)EINVAL;
    }
    struct cell_peer_cred peer;
    if (!cell_fd_unix_peer_cred((int)fd, &peer)) { return -(int64_t)ENOTCONN; }
    struct ucred64 out = {
      .pid = peer.pid,
      .uid = peer.uid,
      .gid = peer.gid,
    };
    uint32_t out_len = sizeof(out);
    if (!vmm_copy_to_user(syscall_active_as(), optval, &out, sizeof(out)) ||
        !vmm_copy_to_user(syscall_active_as(), optlen_addr, &out_len, sizeof(out_len))) {
      return -(int64_t)EFAULT;
    }
    return 0;
  }

  if (level == SOL_SOCKET && (optname == SO_RCVTIMEO_OLD || optname == SO_SNDTIMEO_OLD || optname == SO_RCVTIMEO_NEW ||
                              optname == SO_SNDTIMEO_NEW)) {
    if (optlen < sizeof(struct timeval64) || !syscall_user_writable(optval, sizeof(struct timeval64))) {
      return -(int64_t)EINVAL;
    }
    uint64_t ticks = 0;
    bool receive = optname == SO_RCVTIMEO_OLD || optname == SO_RCVTIMEO_NEW;
    if (!cell_fd_socket_get_timeout((int)fd, receive, &ticks)) { return -(int64_t)EBADF; }
    int rc = socket_copy_timeout_to_user(optval, optlen_addr, ticks);
    return rc < 0 ? (int64_t)rc : 0;
  }

  if (level == SOL_SOCKET && optname == SO_LINGER) {
    if (optlen < sizeof(struct linger64) || !syscall_user_writable(optval, sizeof(struct linger64))) {
      return -(int64_t)EINVAL;
    }
    bool enabled = false;
    uint32_t seconds = 0;
    if (!cell_fd_socket_get_linger((int)fd, &enabled, &seconds)) { return -(int64_t)EBADF; }
    struct linger64 linger = {
      .onoff = enabled ? 1 : 0,
      .linger = (int32_t)seconds,
    };
    uint32_t out_len = sizeof(linger);
    if (!vmm_copy_to_user(syscall_active_as(), optval, &linger, sizeof(linger)) ||
        !vmm_copy_to_user(syscall_active_as(), optlen_addr, &out_len, sizeof(out_len))) {
      return -(int64_t)EFAULT;
    }
    return 0;
  }

  if (optlen < sizeof(int32_t) || !syscall_user_writable(optval, sizeof(int32_t))) { return -(int64_t)EINVAL; }

  int32_t value = 0;
  if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_ERROR:
      value = cell_fd_socket_error((int)fd, true);
      if (value < 0) { return -(int64_t)EBADF; }
      break;
    case SO_TYPE:
      if (!cell_fd_socket_info((int)fd, &value, NULL)) { return -(int64_t)EBADF; }
      break;
    case SO_REUSEADDR:
    case SO_REUSEPORT:
    case SO_DONTROUTE:
    case SO_BROADCAST:
    case SO_SNDBUF:
    case SO_RCVBUF:
    case SO_KEEPALIVE:
    case SO_RCVLOWAT:
    case SO_SNDLOWAT:
      if (cell_fd_socket_get_int_option((int)fd, (int)level, (int)optname, &value) < 0) { return -(int64_t)EBADF; }
      break;
    case SO_ACCEPTCONN: {
      bool accepting = false;
      if (!cell_fd_socket_accepting((int)fd, &accepting)) { return -(int64_t)EBADF; }
      value = accepting ? 1 : 0;
      break;
    }
    case SO_PROTOCOL:
      if (!cell_fd_socket_info((int)fd, NULL, &value)) { return -(int64_t)EBADF; }
      break;
    case SO_DOMAIN: {
      int32_t proto = 0;
      if (!cell_fd_socket_info((int)fd, NULL, &proto)) { return -(int64_t)EBADF; }
      value = proto == 0 ? AF_UNIX : AF_INET;
      break;
    }
    default:
      return -(int64_t)ENOPROTOOPT;
    }
  } else if (level == IPPROTO_TCP) {
    int32_t proto = 0;
    if (!cell_fd_socket_info((int)fd, NULL, &proto)) { return -(int64_t)EBADF; }
    if (proto != IPPROTO_TCP) { return -(int64_t)ENOPROTOOPT; }
    int rc = cell_fd_socket_get_int_option((int)fd, (int)level, (int)optname, &value);
    if (rc < 0) { return (int64_t)rc; }
  } else if (level == IPPROTO_IP) {
    int rc = cell_fd_socket_get_int_option((int)fd, (int)level, (int)optname, &value);
    if (rc < 0) { return (int64_t)rc; }
  } else {
    return -(int64_t)ENOPROTOOPT;
  }

  uint32_t out_len = sizeof(value);
  if (!vmm_copy_to_user(syscall_active_as(), optval, &value, sizeof(value)) ||
      !vmm_copy_to_user(syscall_active_as(), optlen_addr, &out_len, sizeof(out_len))) {
    return -(int64_t)EFAULT;
  }
  return 0;
}

static bool socket_option_readable(uint64_t optval, uint64_t optlen) {
  return optlen == 0 || (optval != 0 && syscall_user_readable(optval, optlen));
}

static int socket_option_int(uint64_t optval, uint64_t optlen, int32_t *out) {
  if (optlen < sizeof(int32_t) || !socket_option_readable(optval, sizeof(int32_t))) { return -EINVAL; }
  return vmm_copy_from_user(syscall_active_as(), out, optval, sizeof(*out)) ? 0 : -EFAULT;
}

static int socket_option_timeval_ticks(uint64_t optval, uint64_t optlen, uint64_t *ticks) {
  if (ticks == NULL) { return -EINVAL; }
  if (optlen != 16 || !socket_option_readable(optval, 16)) { return -EINVAL; }
  struct timeval64 tv;
  if (!vmm_copy_from_user(syscall_active_as(), &tv, optval, sizeof(tv))) { return -EFAULT; }
  if (tv.sec < 0 || tv.usec < 0 || tv.usec >= 1000000) { return -EINVAL; }
  if (tv.sec == 0 && tv.usec == 0) {
    *ticks = 0;
    return 0;
  }
  if ((uint64_t)tv.sec > (UINT64_MAX - 99) / 100) { return -EINVAL; }
  *ticks = (uint64_t)tv.sec * 100 + ((uint64_t)tv.usec + 9999) / 10000;
  if (*ticks == 0) { *ticks = 1; }
  return 0;
}

static int socket_copy_timeout_to_user(uint64_t optval, uint64_t optlen_addr, uint64_t ticks) {
  uint32_t out_len = sizeof(struct timeval64);
  struct timeval64 tv = {
    .sec = (int64_t)(ticks / 100),
    .usec = (int64_t)((ticks % 100) * 10000),
  };
  return vmm_copy_to_user(syscall_active_as(), optval, &tv, sizeof(tv)) &&
             vmm_copy_to_user(syscall_active_as(), optlen_addr, &out_len, sizeof(out_len))
           ? 0
           : -EFAULT;
}

int64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen) {
  int32_t type = 0;
  int32_t proto = 0;
  if (!cell_fd_socket_info((int)fd, &type, &proto)) { return -(int64_t)EBADF; }

  int32_t value = 0;
  if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_REUSEADDR:
    case SO_REUSEPORT:
    case SO_DONTROUTE:
    case SO_BROADCAST:
    case SO_KEEPALIVE:
    case SO_RCVLOWAT:
    case SO_SNDBUF:
    case SO_RCVBUF: {
      int rc = socket_option_int(optval, optlen, &value);
      if (rc < 0) { return rc; }
      rc = cell_fd_socket_set_int_option((int)fd, (int)level, (int)optname, value);
      return rc < 0 ? (int64_t)rc : 0;
    }
    case SO_LINGER: {
      if (optlen < sizeof(struct linger64) || !socket_option_readable(optval, sizeof(struct linger64))) {
        return -(int64_t)EINVAL;
      }
      struct linger64 linger;
      if (!vmm_copy_from_user(syscall_active_as(), &linger, optval, sizeof(linger))) { return -(int64_t)EFAULT; }
      if (linger.linger < 0) { return -(int64_t)EINVAL; }
      return cell_fd_socket_set_linger((int)fd, linger.onoff != 0, (uint32_t)linger.linger) ? 0 : -(int64_t)EBADF;
    }
    case SO_RCVTIMEO_OLD:
    case SO_SNDTIMEO_OLD:
    case SO_RCVTIMEO_NEW:
    case SO_SNDTIMEO_NEW: {
      uint64_t ticks = 0;
      int rc = socket_option_timeval_ticks(optval, optlen, &ticks);
      if (rc < 0) { return rc; }
      bool receive = optname == SO_RCVTIMEO_OLD || optname == SO_RCVTIMEO_NEW;
      return cell_fd_socket_set_timeout((int)fd, receive, ticks) ? 0 : -(int64_t)EBADF;
    }
    default:
      return -(int64_t)ENOPROTOOPT;
    }
  }

  if (level == IPPROTO_TCP) {
    if (proto != IPPROTO_TCP) { return -(int64_t)ENOPROTOOPT; }
    switch (optname) {
    case TCP_NODELAY:
    case TCP_KEEPIDLE:
    case TCP_KEEPINTVL:
    case TCP_KEEPCNT:
    case TCP_USER_TIMEOUT:
    case TCP_FASTOPEN:
    case TCP_FASTOPEN_CONNECT: {
      int rc = socket_option_int(optval, optlen, &value);
      if (rc < 0) { return rc; }
      rc = cell_fd_socket_set_int_option((int)fd, (int)level, (int)optname, value);
      return rc < 0 ? (int64_t)rc : 0;
    }
    default:
      return -(int64_t)ENOPROTOOPT;
    }
  }

  if (level == IPPROTO_IP) {
    switch (optname) {
    case IP_TOS:
    case IP_TTL:
    case IP_MTU_DISCOVER:
    case IP_BIND_ADDRESS_NO_PORT: {
      int rc = socket_option_int(optval, optlen, &value);
      if (rc < 0) { return rc; }
      rc = cell_fd_socket_set_int_option((int)fd, (int)level, (int)optname, value);
      return rc < 0 ? (int64_t)rc : 0;
    }
    default:
      return -(int64_t)ENOPROTOOPT;
    }
  }

  return -(int64_t)ENOPROTOOPT;
}

int64_t sys_shutdown(uint64_t fd, uint64_t how) {
  int rc = cell_fd_socket_shutdown((int)fd, (int)how);
  return rc < 0 ? (int64_t)rc : 0;
}
