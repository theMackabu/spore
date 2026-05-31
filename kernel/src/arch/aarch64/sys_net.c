#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mem.h"
#include "proc/io.h"

#include <stdbool.h>
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
  SO_RCVTIMEO_OLD = 20,
  SO_SNDTIMEO_OLD = 21,
  SO_PROTOCOL = 38,
  SO_RCVTIMEO_NEW = 66,
  SO_SNDTIMEO_NEW = 67,
  IP_TOS = 1,
  IP_MTU_DISCOVER = 10,
  IP_BIND_ADDRESS_NO_PORT = 24,
  IPPROTO_ICMP = 1,
  IPPROTO_TCP = 6,
  IPPROTO_UDP = 17,
  TCP_NODELAY = 1,
  TCP_KEEPIDLE = 4,
  TCP_KEEPINTVL = 5,
  TCP_KEEPCNT = 6,
  TCP_FASTOPEN = 23,
  TCP_FASTOPEN_CONNECT = 30,
  EPERM = 1,
  EBADF = 9,
  EFAULT = 14,
  EINVAL = 22,
  EMSGSIZE = 90,
  EAFNOSUPPORT = 97,
  ENOPROTOOPT = 92,
  EPROTONOSUPPORT = 93,
  ENOTCONN = 107,
  EISCONN = 106,
  EPIPE = 32,
  SIGPIPE = 13,
  MAX_IOVCNT = 1024,
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
    if (tcp == -(int64_t)EPIPE && (flags & MSG_NOSIGNAL) == 0 && frame != NULL &&
        cell_signal_current(SIGPIPE, frame)) {
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
  int64_t rc = cell_fd_socket_recv((int)fd, buf, len, (uint32_t)flags, (flags & MSG_DONTWAIT) != 0, frame, addr,
                                   addrlen);
  return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
}

static int copy_iov_at(uint64_t iov_addr, int32_t iovlen, int32_t index, struct iovec64 *out) {
  if (iovlen < 0 || iovlen > MAX_IOVCNT || index < 0 || index >= iovlen || out == NULL) { return -EINVAL; }
  if (iov_addr == 0 || !syscall_user_readable(iov_addr + (uint64_t)index * sizeof(*out), sizeof(*out))) {
    return -EFAULT;
  }
  return vmm_copy_from_user(syscall_active_as(), out, iov_addr + (uint64_t)index * sizeof(*out), sizeof(*out)) ? 0
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

int64_t sys_sendmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags) {
  if (msg_addr == 0 || !syscall_user_readable(msg_addr, sizeof(struct msghdr64))) { return -(int64_t)EFAULT; }
  struct msghdr64 msg;
  if (!vmm_copy_from_user(syscall_active_as(), &msg, msg_addr, sizeof(msg))) { return -(int64_t)EFAULT; }
  int32_t proto = 0;
  bool is_socket = cell_fd_socket_info((int)fd, NULL, &proto);
  if (is_socket && proto == IPPROTO_TCP) {
    int64_t rc = cell_fd_socket_sendmsg((int)fd, msg_addr, (flags & MSG_DONTWAIT) != 0, frame);
    if (rc == -(int64_t)EPIPE && (flags & MSG_NOSIGNAL) == 0 && frame != NULL &&
        cell_signal_current(SIGPIPE, frame)) {
      return CELL_SWITCHED;
    }
    return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
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
  (void)flags;
  if (msg_addr == 0 || !syscall_user_readable(msg_addr, sizeof(struct msghdr64))) { return -(int64_t)EFAULT; }
  int32_t proto = 0;
  if (cell_fd_socket_info((int)fd, NULL, &proto) && (proto == IPPROTO_TCP || proto == IPPROTO_UDP || proto == IPPROTO_ICMP)) {
    int64_t rc = cell_fd_socket_recvmsg((int)fd, msg_addr, (uint32_t)flags, (flags & MSG_DONTWAIT) != 0, frame);
    return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
  }
  struct msghdr64 msg;
  if (!vmm_copy_from_user(syscall_active_as(), &msg, msg_addr, sizeof(msg))) { return -(int64_t)EFAULT; }
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

  if (level == SOL_SOCKET &&
      (optname == SO_RCVTIMEO_OLD || optname == SO_SNDTIMEO_OLD || optname == SO_RCVTIMEO_NEW ||
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
      if (cell_fd_socket_get_int_option((int)fd, (int)level, (int)optname, &value) < 0) { return -(int64_t)EBADF; }
      break;
    case SO_PROTOCOL:
      if (!cell_fd_socket_info((int)fd, NULL, &value)) { return -(int64_t)EBADF; }
      break;
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
    case SO_SNDBUF:
    case SO_RCVBUF: {
      int rc = socket_option_int(optval, optlen, &value);
      if (rc < 0) { return rc; }
      rc = cell_fd_socket_set_int_option((int)fd, (int)level, (int)optname, value);
      return rc < 0 ? (int64_t)rc : 0;
    }
    case SO_LINGER:
      if (optlen < sizeof(struct linger64) || !socket_option_readable(optval, sizeof(struct linger64))) {
        return -(int64_t)EINVAL;
      }
      return 0;
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
