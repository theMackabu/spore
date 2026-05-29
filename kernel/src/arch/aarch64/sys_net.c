#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mem.h"
#include "proc/io.h"

#include <stdbool.h>
#include <stdint.h>

enum {
  AF_UNIX = 1,
  AF_INET = 2,
  SOCK_STREAM = 1,
  SOCK_DGRAM = 2,
  SOL_SOCKET = 1,
  SO_TYPE = 3,
  SO_ERROR = 4,
  SO_SNDBUF = 7,
  SO_RCVBUF = 8,
  SO_KEEPALIVE = 9,
  SO_PEERCRED = 17,
  SO_PROTOCOL = 38,
  IPPROTO_ICMP = 1,
  IPPROTO_TCP = 6,
  IPPROTO_UDP = 17,
  TCP_NODELAY = 1,
  EPERM = 1,
  EBADF = 9,
  EFAULT = 14,
  EINVAL = 22,
  EAFNOSUPPORT = 97,
  ENOPROTOOPT = 92,
  EPROTONOSUPPORT = 93,
  ENOTCONN = 107,
  MAX_IOVCNT = 1024,
  F_GETFD = 1,
  FD_CLOEXEC = 1,
  O_NONBLOCK = 04000,
  O_CLOEXEC = 02000000,
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

static uint16_t bswap16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

static bool copy_sockaddr_in(uint64_t addr, uint64_t len, struct sockaddr_in64 *out) {
  if (addr == 0 || len < sizeof(*out) || !syscall_user_readable(addr, sizeof(*out))) { return false; }
  return vmm_copy_from_user(syscall_active_as(), out, addr, sizeof(*out)) && out->sin_family == AF_INET;
}

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
  return cell_fd_udp_bind((int)fd, bswap16(sa.sin_port)) ? 0 : -(int64_t)EBADF;
}

int64_t sys_connect(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t len) {
  uint16_t family = 0;
  if (!copy_sockaddr_family(addr, len, &family)) { return -(int64_t)EINVAL; }
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
  int rc = cell_fd_unix_listen((int)fd, (int)backlog);
  return rc < 0 ? (int64_t)rc : 0;
}

int64_t sys_accept(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t addrlen) {
  (void)addr;
  (void)addrlen;
  int rc = cell_fd_unix_accept((int)fd, frame);
  return rc == CELL_SWITCHED ? CELL_SWITCHED : (int64_t)rc;
}

int64_t sys_sendto(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                   uint64_t addrlen) {
  (void)flags;
  if (!syscall_user_readable(buf, len)) { return -(int64_t)EFAULT; }
  if (addr == 0) {
    int64_t tcp = cell_fd_tcp_send((int)fd, buf, len);
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
  (void)flags;
  if (!syscall_user_writable(buf, len)) { return -(int64_t)EFAULT; }
  int64_t rc = cell_fd_socket_recv((int)fd, buf, len, frame, addr, addrlen);
  return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
}

static bool copy_first_iov(uint64_t iov_addr, int32_t iovlen, struct iovec64 *out) {
  if (iovlen <= 0 || iovlen > MAX_IOVCNT || iov_addr == 0 || !syscall_user_readable(iov_addr, sizeof(*out))) {
    return false;
  }
  return vmm_copy_from_user(syscall_active_as(), out, iov_addr, sizeof(*out));
}

int64_t sys_sendmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags) {
  if (msg_addr == 0 || !syscall_user_readable(msg_addr, sizeof(struct msghdr64))) { return -(int64_t)EFAULT; }
  struct msghdr64 msg;
  if (!vmm_copy_from_user(syscall_active_as(), &msg, msg_addr, sizeof(msg))) { return -(int64_t)EFAULT; }
  struct iovec64 iov;
  if (!copy_first_iov(msg.iov, msg.iovlen, &iov)) { return -(int64_t)EINVAL; }
  return sys_sendto(frame, fd, iov.base, iov.len, flags, msg.name, msg.namelen);
}

int64_t sys_recvmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags) {
  if (msg_addr == 0 || !syscall_user_readable(msg_addr, sizeof(struct msghdr64))) { return -(int64_t)EFAULT; }
  struct msghdr64 msg;
  if (!vmm_copy_from_user(syscall_active_as(), &msg, msg_addr, sizeof(msg))) { return -(int64_t)EFAULT; }
  struct iovec64 iov;
  if (!copy_first_iov(msg.iov, msg.iovlen, &iov)) { return -(int64_t)EINVAL; }
  int64_t rc = sys_recvfrom(frame, fd, iov.base, iov.len, flags, msg.name, msg_addr + 8);
  if (rc >= 0) {
    msg.flags = 0;
    (void)vmm_copy_to_user(syscall_active_as(), msg_addr, &msg, sizeof(msg));
  }
  return rc;
}

int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen) {
  (void)fd;
  if (addr == 0 || addrlen == 0 || !syscall_user_writable(addrlen, sizeof(uint32_t))) { return -(int64_t)EFAULT; }
  uint32_t len = sizeof(struct sockaddr_in64);
  (void)vmm_copy_to_user(syscall_active_as(), addrlen, &len, sizeof(len));
  if (!syscall_user_writable(addr, sizeof(struct sockaddr_in64))) { return -(int64_t)EFAULT; }
  struct sockaddr_in64 sa = {.sin_family = AF_INET};
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

  if (optlen < sizeof(int32_t) || !syscall_user_writable(optval, sizeof(int32_t))) { return -(int64_t)EINVAL; }

  int32_t value = 0;
  if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_ERROR:
      value = 0;
      break;
    case SO_TYPE:
      if (!cell_fd_socket_info((int)fd, &value, NULL)) { return -(int64_t)EBADF; }
      break;
    case SO_SNDBUF:
    case SO_RCVBUF:
      value = 262144;
      break;
    case SO_KEEPALIVE:
      value = 0;
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
    if (optname != TCP_NODELAY || proto != IPPROTO_TCP) { return -(int64_t)ENOPROTOOPT; }
    value = 1;
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
