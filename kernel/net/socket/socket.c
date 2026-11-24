#include "internal.h"

#include "proc/accounting.h"
#include "net/net.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/thread.h"

enum {
  SOL_SOCKET = 1,
  SOCK_STREAM = 1,
  SOCK_DGRAM = 2,
  IPPROTO_IP = 0,
  SO_REUSEADDR = 2,
  SO_DONTROUTE = 5,
  SO_BROADCAST = 6,
  SO_SNDBUF = 7,
  SO_RCVBUF = 8,
  SO_KEEPALIVE = 9,
  SO_REUSEPORT = 15,
  SO_RCVLOWAT = 18,
  SO_SNDLOWAT = 19,
  IP_TOS = 1,
  IP_TTL = 2,
  IP_RECVTTL = 12,
  IP_MTU_DISCOVER = 10,
  IP_BIND_ADDRESS_NO_PORT = 24,
  TCP_NODELAY = 1,
  TCP_KEEPIDLE = 4,
  TCP_KEEPINTVL = 5,
  TCP_KEEPCNT = 6,
  TCP_USER_TIMEOUT = 18,
  TCP_FASTOPEN = 23,
  TCP_FASTOPEN_CONNECT = 30,
  ENOPROTOOPT = 92,
  DEFAULT_SOCKET_BUFFER = 262144,
  TCP_SEND_BUFFER_CAP = CELL_TCP_TX_CAP * CELL_TCP_TX_QUEUE_CAP,
  DGRAM_RECEIVE_BUFFER_CAP = CELL_DGRAM_RX_CAP * CELL_DGRAM_RX_QUEUE_CAP,
};

uint16_t net_bswap16(uint16_t v) {
  return (uint16_t)((v << 8) | (v >> 8));
}

static uint32_t socket_buffer_cap(const struct open_file *file, bool receive) {
  if (file == NULL) { return DEFAULT_SOCKET_BUFFER; }
  if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
    return receive ? CELL_TCP_RX_CAP : TCP_SEND_BUFFER_CAP;
  }
  if (file->type == OPEN_SOCKET && (file->socket_proto == IPPROTO_UDP || file->socket_proto == IPPROTO_ICMP)) {
    return receive ? DGRAM_RECEIVE_BUFFER_CAP : DEFAULT_SOCKET_BUFFER;
  }
  return DEFAULT_SOCKET_BUFFER;
}

uint32_t socket_effective_buffer(const struct open_file *file, bool receive) {
  if (file == NULL) { return DEFAULT_SOCKET_BUFFER; }
  uint32_t configured = receive ? file->so_rcvbuf : file->so_sndbuf;
  uint32_t cap = socket_buffer_cap(file, receive);
  if (configured == 0) { return cap; }
  return configured > cap ? cap : configured;
}

uint32_t socket_effective_rcvlowat(const struct open_file *file) {
  return file == NULL || file->so_rcvlowat == 0 ? 1 : file->so_rcvlowat;
}

uint8_t socket_effective_ttl(const struct open_file *file) {
  return file == NULL || file->ip_ttl == 0 ? 64 : (uint8_t)file->ip_ttl;
}

int cell_socket_poll_events(const struct open_file *file, int events) {
  switch (file->type) {
  case OPEN_SOCKET:
    return file->socket_proto == IPPROTO_TCP ? tcp_poll_events(file, events) : dgram_poll_events(file, events);
  case OPEN_UNIX_LISTENER:
  case OPEN_UNIX_STREAM:
    return unix_poll_events(file, events);
  default:
    return CELL_POLLNVAL;
  }
}

int cell_fd_socket_inet(uint8_t proto) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_SOCKET;
  file->socket_proto = proto;
  if (proto == IPPROTO_TCP) { tcp_init_socket(file); }
  domain->fds[fd] = file;
  domain->fd_flags[fd] = 0;
  return fd;
}

bool cell_fd_socket_info(int fd, int32_t *type, int32_t *proto) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER) {
    if (type != NULL) { *type = SOCK_STREAM; }
    if (proto != NULL) { *proto = 0; }
    return true;
  }
  if (file->type != OPEN_SOCKET) { return false; }
  if (type != NULL) { *type = file->socket_proto == IPPROTO_TCP ? SOCK_STREAM : SOCK_DGRAM; }
  if (proto != NULL) { *proto = file->socket_proto; }
  return true;
}

int cell_fd_socket_error(int fd, bool clear) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
    int err = file->tcp_error;
    if (clear) { file->tcp_error = 0; }
    return err;
  }
  if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_UDP) {
    int err = file->udp_error;
    if (clear) { file->udp_error = 0; }
    return err;
  }
  if (file->type == OPEN_SOCKET || file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER) { return 0; }
  return -9;
}

int cell_fd_socket_get_int_option(int fd, int level, int optname, int32_t *out) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || out == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET && file->type != OPEN_UNIX_STREAM && file->type != OPEN_UNIX_LISTENER) { return -9; }
  if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_REUSEADDR:
      *out = file->so_reuseaddr ? 1 : 0;
      return 0;
    case SO_REUSEPORT:
      *out = file->so_reuseport ? 1 : 0;
      return 0;
    case SO_DONTROUTE:
      *out = file->so_dontroute ? 1 : 0;
      return 0;
    case SO_BROADCAST:
      *out = file->so_broadcast ? 1 : 0;
      return 0;
    case SO_KEEPALIVE:
      *out = file->so_keepalive ? 1 : 0;
      return 0;
    case SO_RCVLOWAT:
      *out = (int32_t)socket_effective_rcvlowat(file);
      return 0;
    case SO_SNDLOWAT:
      *out = 1;
      return 0;
    case SO_SNDBUF:
      *out = (int32_t)socket_effective_buffer(file, false);
      return 0;
    case SO_RCVBUF:
      *out = (int32_t)socket_effective_buffer(file, true);
      return 0;
    default:
      return -ENOPROTOOPT;
    }
  }
  if (level == IPPROTO_TCP) {
    if (file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -ENOPROTOOPT; }
    switch (optname) {
    case TCP_NODELAY:
      *out = file->tcp_nodelay ? 1 : 0;
      return 0;
    case TCP_KEEPIDLE:
      *out = (int32_t)file->tcp_keepidle;
      return 0;
    case TCP_KEEPINTVL:
      *out = (int32_t)file->tcp_keepintvl;
      return 0;
    case TCP_KEEPCNT:
      *out = (int32_t)file->tcp_keepcnt;
      return 0;
    case TCP_USER_TIMEOUT:
      *out = (int32_t)file->tcp_user_timeout_ms;
      return 0;
    default:
      return -ENOPROTOOPT;
    }
  }
  if (level == IPPROTO_IP) {
    if (file->type != OPEN_SOCKET) { return -ENOPROTOOPT; }
    switch (optname) {
    case IP_TOS:
      *out = (int32_t)file->ip_tos;
      return 0;
    case IP_RECVTTL:
      *out = file->ip_recvttl;
      return 0;
    case IP_TTL:
      *out = (int32_t)socket_effective_ttl(file);
      return 0;
    case IP_MTU_DISCOVER:
      *out = (int32_t)file->ip_mtu_discover;
      return 0;
    case IP_BIND_ADDRESS_NO_PORT:
      *out = file->ip_bind_address_no_port ? 1 : 0;
      return 0;
    default:
      return -ENOPROTOOPT;
    }
  }
  return -ENOPROTOOPT;
}

bool cell_fd_socket_get_linger(int fd, bool *enabled, uint32_t *seconds) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || enabled == NULL || seconds == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
    return false;
  }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET && file->type != OPEN_UNIX_STREAM && file->type != OPEN_UNIX_LISTENER) { return false; }
  *enabled = file->so_linger_enabled;
  *seconds = file->so_linger_seconds;
  return true;
}

int cell_fd_socket_set_int_option(int fd, int level, int optname, int32_t value) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET && file->type != OPEN_UNIX_STREAM && file->type != OPEN_UNIX_LISTENER) { return -9; }
  if (level == SOL_SOCKET) {
    switch (optname) {
    case SO_REUSEADDR:
      file->so_reuseaddr = value != 0;
      return 0;
    case SO_REUSEPORT:
      file->so_reuseport = value != 0;
      return 0;
    case SO_DONTROUTE:
      file->so_dontroute = value != 0;
      return 0;
    case SO_BROADCAST:
      file->so_broadcast = value != 0;
      return 0;
    case SO_KEEPALIVE:
      file->so_keepalive = value != 0;
      return 0;
    case SO_SNDBUF:
      if (value <= 0) { return -EINVAL; }
      file->so_sndbuf =
        (uint32_t)value > socket_buffer_cap(file, false) ? socket_buffer_cap(file, false) : (uint32_t)value;
      return 0;
    case SO_RCVBUF:
      if (value <= 0) { return -EINVAL; }
      file->so_rcvbuf =
        (uint32_t)value > socket_buffer_cap(file, true) ? socket_buffer_cap(file, true) : (uint32_t)value;
      return 0;
    case SO_RCVLOWAT:
      if (value <= 0) { return -EINVAL; }
      file->so_rcvlowat = (uint32_t)value;
      return 0;
    default:
      return -ENOPROTOOPT;
    }
  }
  if (level == IPPROTO_TCP) {
    if (file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -ENOPROTOOPT; }
    switch (optname) {
    case TCP_NODELAY:
      file->tcp_nodelay = value != 0;
      return 0;
    case TCP_KEEPIDLE:
      if (value <= 0) { return -EINVAL; }
      file->tcp_keepidle = (uint32_t)value;
      return 0;
    case TCP_KEEPINTVL:
      if (value <= 0) { return -EINVAL; }
      file->tcp_keepintvl = (uint32_t)value;
      return 0;
    case TCP_KEEPCNT:
      if (value <= 0 || value > UINT8_MAX) { return -EINVAL; }
      file->tcp_keepcnt = (uint32_t)value;
      return 0;
    case TCP_USER_TIMEOUT:
      if (value < 0) { return -EINVAL; }
      file->tcp_user_timeout_ms = (uint32_t)value;
      return 0;
    case TCP_FASTOPEN:
    case TCP_FASTOPEN_CONNECT:
      return 0;
    default:
      return -ENOPROTOOPT;
    }
  }
  if (level == IPPROTO_IP) {
    if (file->type != OPEN_SOCKET) { return -ENOPROTOOPT; }
    switch (optname) {
    case IP_TOS:
      if (value < 0 || value > 255) { return -EINVAL; }
      file->ip_tos = (uint32_t)value;
      return 0;
    case IP_RECVTTL:
      file->ip_recvttl = value != 0;
      return 0;
    case IP_TTL:
      if (value <= 0 || value > 255) { return -EINVAL; }
      file->ip_ttl = (uint32_t)value;
      return 0;
    case IP_MTU_DISCOVER:
      file->ip_mtu_discover = (uint32_t)value;
      return 0;
    case IP_BIND_ADDRESS_NO_PORT:
      file->ip_bind_address_no_port = value != 0;
      return 0;
    default:
      return -ENOPROTOOPT;
    }
  }
  return -ENOPROTOOPT;
}

bool cell_fd_socket_set_linger(int fd, bool enabled, uint32_t seconds) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET && file->type != OPEN_UNIX_STREAM && file->type != OPEN_UNIX_LISTENER) { return false; }
  file->so_linger_enabled = enabled;
  file->so_linger_seconds = seconds;
  return true;
}

bool cell_fd_socket_accepting(int fd, bool *accepting) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || accepting == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  *accepting = (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP && file->tcp_state == TCP_LISTEN) ||
               file->type == OPEN_UNIX_LISTENER;
  return file->type == OPEN_SOCKET || file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER;
}

bool cell_fd_socket_get_timeout(int fd, bool receive, uint64_t *ticks) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || ticks == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET && file->type != OPEN_UNIX_STREAM && file->type != OPEN_UNIX_LISTENER) { return false; }
  *ticks = receive ? file->so_rcvtimeo_ticks : file->so_sndtimeo_ticks;
  return true;
}

bool cell_fd_socket_set_timeout(int fd, bool receive, uint64_t ticks) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET && file->type != OPEN_UNIX_STREAM && file->type != OPEN_UNIX_LISTENER) { return false; }
  if (receive) {
    file->so_rcvtimeo_ticks = ticks;
  } else {
    file->so_sndtimeo_ticks = ticks;
  }
  return true;
}

bool cell_fd_socket_local_addr(int fd, uint32_t *ip, uint16_t *port) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET) { return false; }
  struct net_config cfg;
  net_get_config(&cfg);
  if (ip != NULL) { *ip = cfg.local_ip; }
  if (port != NULL) {
    if (file->socket_proto == IPPROTO_TCP) {
      *port = file->tcp_local_port;
    } else {
      *port = file->udp_local_port;
    }
  }
  return true;
}

bool cell_fd_socket_peer_addr(int fd, uint32_t *ip, uint16_t *port) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET) { return false; }
  if (file->socket_proto == IPPROTO_TCP) {
    if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return false; }
    if (ip != NULL) { *ip = file->tcp_remote_ip; }
    if (port != NULL) { *port = file->tcp_remote_port; }
    return true;
  }
  if (file->socket_proto == IPPROTO_UDP && file->udp_connected) {
    if (ip != NULL) { *ip = file->udp_remote_ip; }
    if (port != NULL) { *port = file->udp_remote_port; }
    return true;
  }
  return false;
}

void cell_socket_release_file(struct open_file *file) {
  if (file == NULL || file->type != OPEN_SOCKET) { return; }
  if (file->socket_proto == IPPROTO_TCP) { tcp_release_file(file); }
  if (file->socket_proto == IPPROTO_UDP || file->socket_proto == IPPROTO_ICMP) { udp_release_file(file); }
}

void socket_apply_accept_flags(struct domain *domain, int fd, int flags) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return; }
  if ((flags & CELL_O_NONBLOCK) != 0) { domain->fds[fd]->flags |= CELL_O_NONBLOCK; }
  if ((flags & CELL_O_CLOEXEC) != 0) { domain->fd_flags[fd] = 1; }
}

void socket_close_domain_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return; }
  cell_release_open_file(domain->fds[fd]);
  domain->fds[fd] = NULL;
  domain->fd_flags[fd] = 0;
}

int64_t cell_fd_socket_recv(int fd, uint64_t buf, uint64_t len, uint32_t flags, bool dontwait, struct trap_frame *frame,
                            uint64_t addr, uint64_t addrlen) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_UNIX_STREAM) {
    if (!file->unix_connected) { return -ENOTCONN; }
    int64_t got = cell_pipe_read_id_to_domain(domain, file->unix_rx_pipe, buf, len);
    if (got != -EAGAIN || dontwait || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return got; }
    return cell_block_current_on_pipe(fd, buf, len, false, frame);
  }
  if (file->type != OPEN_SOCKET) { return -9; }
  net_poll();
  bool can_block = !dontwait && (file->flags & CELL_O_NONBLOCK) == 0 && frame != NULL;
  if (file->socket_proto == IPPROTO_TCP) {
    if (can_block && tcp_recv_waitall_pending(file, len, flags)) {
      return cell_block_current_on_socket_flags_timeout(fd, buf, len, 0, 0, flags, file->so_rcvtimeo_ticks, frame);
    }
    int64_t got = cell_socket_tcp_read_to_domain_flags(domain, file, buf, len, flags);
    if (got != -EAGAIN || !can_block) { return got; }
    return cell_block_current_on_socket_flags_timeout(fd, buf, len, 0, 0, flags, file->so_rcvtimeo_ticks, frame);
  }
  if (file->socket_proto == IPPROTO_UDP && file->udp_error != 0) {
    int err = file->udp_error;
    file->udp_error = 0;
    return -(int64_t)err;
  }
  if (file->dgram_rx_count == 0) {
    if (dontwait || (file->flags & CELL_O_NONBLOCK) != 0) { return -EAGAIN; }
    if (frame == NULL) { return -EAGAIN; }
    return cell_block_current_on_socket_flags_timeout(fd, buf, len, addr, addrlen, flags, file->so_rcvtimeo_ticks,
                                                      frame);
  }
  return dgram_read_to_domain(domain, file, buf, len, addr, addrlen, flags);
}

bool cell_fd_is_datagram(int fd) {
  int32_t type;
  return cell_fd_socket_info(fd, &type, NULL) && type == SOCK_DGRAM;
}

static void wake_socket_timeout_waiters(uint64_t now_ticks) {
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_SOCKET, &cursor))) {
    if (!thread->wait.has_deadline || now_ticks < thread->wait.deadline_tick) {
      continue;
    }
    cell_finish_wait(thread, -EAGAIN);
  }
}

void wake_socket_waiters(struct open_file *file) {
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_SOCKET, &cursor))) {
    int fd = thread->wait.target;
    if (fd >= 0 && fd < MAX_FDS && thread->domain->fds[fd] == file) {
      if (thread->wait.write && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
        int64_t rc = thread->wait.socket_msg
                       ? socket_sendmsg_tcp_from_domain(thread->domain, file, thread->wait.socket_msg_addr)
                       : cell_socket_tcp_write_from_domain(thread->domain, file, thread->wait.buf, thread->wait.len);
        if (rc == -EAGAIN && tcp_send_would_block(file)) { continue; }
        cell_finish_wait(thread, rc);
      } else if (thread->wait.socket_msg && file->type == OPEN_SOCKET) {
        int64_t rc = socket_recvmsg_to_iov(thread->domain, file, thread->wait.socket_msg_addr, thread->wait.socket_iov,
                                           (int32_t)thread->wait.socket_iovlen, thread->wait.socket_addr, thread->wait.socket_addrlen,
                                           thread->wait.socket_flags);
        if (rc == -EAGAIN) { continue; }
        cell_finish_wait(thread, rc);
      } else if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
        if (thread->wait.buf == 0 && thread->wait.len == 0) {
          if (file->tcp_state == TCP_LISTEN) {
            int accepted = cell_socket_take_pending_tcp(thread->domain, file, thread->wait.socket_accept_flags);
            if (accepted == -EAGAIN) { continue; }
            if (accepted < 0) {
              cell_finish_wait(thread, accepted);
              continue;
            }
            int addr_rc =
              tcp_copy_peer_addr_to_domain(thread->domain, accepted, thread->wait.socket_addr, thread->wait.socket_addrlen);
            if (addr_rc < 0) {
              socket_close_domain_fd(thread->domain, accepted);
              cell_finish_wait(thread, addr_rc);
              continue;
            }
            socket_apply_accept_flags(thread->domain, accepted, thread->wait.socket_accept_flags);
            cell_finish_wait(thread, accepted);
          } else if (file->tcp_error != 0) {
            cell_finish_wait(thread, -(int64_t)file->tcp_error);
          } else if (file->tcp_state != TCP_ESTABLISHED) {
            continue;
          } else {
            cell_finish_wait(thread, 0);
          }
        } else {
          if (tcp_recv_waitall_pending(file, thread->wait.len, thread->wait.socket_flags)) { continue; }
          int64_t rc = cell_socket_tcp_read_to_domain_flags(thread->domain, file, thread->wait.buf, thread->wait.len,
                                                            thread->wait.socket_flags);
          if (rc == -EAGAIN) { continue; }
          cell_finish_wait(thread, rc);
        }
      } else if (file->type == OPEN_SOCKET && (file->socket_proto == IPPROTO_UDP || file->socket_proto == IPPROTO_ICMP) &&
                 (file->dgram_rx_count != 0 || file->udp_error != 0)) {
        int64_t rc = dgram_read_to_domain(thread->domain, file, thread->wait.buf, thread->wait.len, thread->wait.socket_addr,
                                          thread->wait.socket_addrlen, thread->wait.socket_flags);
        if (rc == -EAGAIN) { continue; }
        cell_finish_wait(thread, rc);
      }
    }
  }
  cell_wake_poll_waiters_internal();
}

void cell_socket_wake_file(struct open_file *file) {
  wake_socket_waiters(file);
}

void cell_socket_timer_tick(uint64_t now_ticks) {
  wake_socket_timeout_waiters(now_ticks);
  tcp_timer_tick(now_ticks);
}
