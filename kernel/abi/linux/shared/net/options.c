#include "abi/linux/shared/socket_options.h"
#include "abi/linux/shared/usercopy.h"
#include "handlers.h"
#include "proc/domain.h"
#include "proc/user_namespace.h"
#include "socket_abi.h"
#include "abi/linux/shared/layout.h"
#include "net/socket.h"
#include "net/route.h"
#include "proc/fd.h"

constexpr int ENOPROTOOPT = 92;

struct ucred64 {
  int32_t pid;
  uint32_t uid;
  uint32_t gid;
};

struct linger64 {
  int32_t onoff;
  int32_t linger;
};

static int socket_copy_timeout_to_user(uint64_t optval, uint64_t optlen_addr, uint64_t ticks);

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
      .uid = userns_visible(cell_current_domain_internal()->user_ns, false, peer.uid),
      .gid = userns_visible(cell_current_domain_internal()->user_ns, true, peer.gid),
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
      struct open_file *file = cell_fd_file((int)fd);
      value = file && file->type == OPEN_SOCKET && file->socket_proto == ROUTE_PROTOCOL ? ROUTE_FAMILY :
              proto == 0 ? AF_UNIX : AF_INET;
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
  if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000) { return -EINVAL; }
  if (tv.tv_sec == 0 && tv.tv_usec == 0) {
    *ticks = 0;
    return 0;
  }
  if ((uint64_t)tv.tv_sec > (UINT64_MAX - 99) / 100) { return -EINVAL; }
  *ticks = (uint64_t)tv.tv_sec * 100 + ((uint64_t)tv.tv_usec + 9999) / 10000;
  if (*ticks == 0) { *ticks = 1; }
  return 0;
}

static int socket_copy_timeout_to_user(uint64_t optval, uint64_t optlen_addr, uint64_t ticks) {
  uint32_t out_len = sizeof(struct timeval64);
  struct timeval64 tv = {
    .tv_sec = (int64_t)(ticks / 100),
    .tv_usec = (int64_t)((ticks % 100) * 10000),
  };
  return vmm_copy_to_user(syscall_active_as(), optval, &tv, sizeof(tv)) &&
             vmm_copy_to_user(syscall_active_as(), optlen_addr, &out_len, sizeof(out_len))
           ? 0
           : -EFAULT;
}

static int set_int_option(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen) {
  int32_t value;
  int rc = socket_option_int(optval, optlen, &value);
  if (rc < 0) { return rc; }
  rc = cell_fd_socket_set_int_option((int)fd, (int)level, (int)optname, value);
  return rc < 0 ? rc : 0;
}

int64_t sys_setsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen) {
  int32_t type = 0;
  int32_t proto = 0;
  if (!cell_fd_socket_info((int)fd, &type, &proto)) { return -(int64_t)EBADF; }

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
      return set_int_option(fd, level, optname, optval, optlen);
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
      return set_int_option(fd, level, optname, optval, optlen);
    }
    default:
      return -(int64_t)ENOPROTOOPT;
    }
  }

  if (level == IPPROTO_IP) {
    switch (optname) {
    case IP_TOS:
    case IP_TTL:
    case IP_RECVTTL:
    case IP_MTU_DISCOVER:
    case IP_BIND_ADDRESS_NO_PORT: {
      return set_int_option(fd, level, optname, optval, optlen);
    }
    default:
      return -(int64_t)ENOPROTOOPT;
    }
  }

  return -(int64_t)ENOPROTOOPT;
}
