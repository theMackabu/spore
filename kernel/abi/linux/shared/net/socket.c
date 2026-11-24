#include "abi/linux/shared/fs/path.h"
#include "abi/linux/shared/usercopy.h"
#include "handlers.h"
#include "socket_abi.h"

#include "lib/mem.h"
#include "proc/fd.h"
#include "proc/io.h"
#include "proc/policy.h"
#include "proc/signal.h"
#include "net/socket.h"
#include "proc/thread.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  AT_FDCWD = -100,
  AF_UNSPEC = 0,
  SOCK_STREAM = 1,
  SOCK_DGRAM = 2,
  EPERM = 1,
  EAFNOSUPPORT = 97,
  EPROTONOSUPPORT = 93,
  O_NONBLOCK = 04000,
  O_CLOEXEC = 02000000,
};

struct sockaddr_un64 {
  uint16_t sun_family;
  char sun_path[108];
};

bool linux_copy_sockaddr_in(uint64_t addr, uint64_t len, struct sockaddr_in64 *out) {
  if (addr == 0 || len < sizeof(*out) || !syscall_user_readable(addr, sizeof(*out))) { return false; }
  return vmm_copy_from_user(syscall_active_as(), out, addr, sizeof(*out)) && out->sin_family == AF_INET;
}

static bool copy_sockaddr_family(uint64_t addr, uint64_t len, uint16_t *family) {
  if (addr == 0 || len < sizeof(uint16_t) || !syscall_user_readable(addr, sizeof(uint16_t))) { return false; }
  return vmm_copy_from_user(syscall_active_as(), family, addr, sizeof(*family));
}

static int unix_socket_path(uint64_t addr, uint64_t len, char path[CELL_PATH_MAX]) {
  struct sockaddr_un64 sa = {0};
  if (len < 3 || len > sizeof(sa)) { return -EINVAL; }
  if (!addr || !syscall_user_readable(addr, len) ||
      !vmm_copy_from_user(syscall_active_as(), &sa, addr, len)) { return -EFAULT; }
  if (sa.sun_family != AF_UNIX || !sa.sun_path[0]) { return -EINVAL; }
  char name[sizeof(sa.sun_path) + 1];
  kmemcpy(name, sa.sun_path, sizeof(sa.sun_path));
  name[sizeof(sa.sun_path)] = 0;
  return (int)syscall_resolve_path_at(AT_FDCWD, name, path, CELL_PATH_MAX);
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
    char path[CELL_PATH_MAX];
    int rc = unix_socket_path(addr, len, path);
    if (!rc) { rc = syscall_may_change_entry(path, false); }
    return rc ? rc : cell_fd_unix_bind((int)fd, path);
  }
  struct sockaddr_in64 sa;
  if (!linux_copy_sockaddr_in(addr, len, &sa)) { return -(int64_t)EINVAL; }
  int rc = cell_fd_tcp_bind((int)fd, __builtin_bswap16(sa.sin_port));
  if (rc == -EBADF) { rc = cell_fd_udp_bind((int)fd, __builtin_bswap16(sa.sin_port)); }
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
    char path[CELL_PATH_MAX];
    int rc = unix_socket_path(addr, len, path);
    return rc ? rc : cell_fd_unix_connect((int)fd, path);
  }
  struct sockaddr_in64 sa;
  if (!linux_copy_sockaddr_in(addr, len, &sa)) { return -(int64_t)EINVAL; }
  int rc = cell_fd_tcp_connect((int)fd, sa.sin_addr, __builtin_bswap16(sa.sin_port), frame);
  if (rc == CELL_SWITCHED) { return CELL_SWITCHED; }
  if (rc != -9) { return (int64_t)rc; }
  if (!cell_egress_allowed(IPPROTO_UDP, sa.sin_addr, __builtin_bswap16(sa.sin_port))) { return -(int64_t)EPERM; }
  return cell_fd_udp_connect((int)fd, sa.sin_addr, __builtin_bswap16(sa.sin_port)) ? 0 : -(int64_t)EBADF;
}

int64_t sys_listen(uint64_t fd, uint64_t backlog) {
  int rc = cell_fd_tcp_listen((int)fd, (int)backlog);
  if (rc == -EBADF) { rc = cell_fd_unix_listen((int)fd, (int)backlog); }
  return rc < 0 ? (int64_t)rc : 0;
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
    if (tcp != -EBADF) { return tcp; }
    int64_t udp = cell_fd_udp_send((int)fd, 0, 0, buf, len);
    if (udp != -EBADF) { return udp; }
    return cell_fd_write((int)fd, buf, len, frame);
  }
  uint32_t ip = 0;
  uint16_t port = 0;
  struct sockaddr_in64 sa;
  if (!linux_copy_sockaddr_in(addr, addrlen, &sa)) { return -(int64_t)EINVAL; }
  ip = sa.sin_addr;
  port = __builtin_bswap16(sa.sin_port);
  return cell_fd_udp_send((int)fd, ip, port, buf, len);
}

int64_t sys_recvfrom(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr,
                     uint64_t addrlen) {
  if (!syscall_user_writable(buf, len)) { return -(int64_t)EFAULT; }
  int64_t rc =
    cell_fd_socket_recv((int)fd, buf, len, (uint32_t)flags, (flags & MSG_DONTWAIT) != 0, frame, addr, addrlen);
  return rc == CELL_SWITCHED ? CELL_SWITCHED : rc;
}

int64_t sys_socket_name(uint64_t fd, uint64_t addr, uint64_t addrlen, bool peer) {
  if (addr == 0 || addrlen == 0 || !syscall_user_writable(addrlen, sizeof(uint32_t))) { return -(int64_t)EFAULT; }
  uint32_t len = sizeof(struct sockaddr_in64);
  (void)vmm_copy_to_user(syscall_active_as(), addrlen, &len, sizeof(len));
  if (!syscall_user_writable(addr, sizeof(struct sockaddr_in64))) { return -(int64_t)EFAULT; }
  uint32_t ip = 0;
  uint16_t port = 0;
  if (peer) {
    if (!cell_fd_socket_peer_addr((int)fd, &ip, &port)) { return -ENOTCONN; }
  } else if (!cell_fd_socket_local_addr((int)fd, &ip, &port)) { return -EBADF; }
  struct sockaddr_in64 sa = {
    .sin_family = AF_INET,
    .sin_port = __builtin_bswap16(port),
    .sin_addr = ip,
  };
  return vmm_copy_to_user(syscall_active_as(), addr, &sa, sizeof(sa)) ? 0 : -(int64_t)EFAULT;
}

int64_t sys_shutdown(uint64_t fd, uint64_t how) {
  int rc = cell_fd_socket_shutdown((int)fd, (int)how);
  return rc < 0 ? (int64_t)rc : 0;
}
