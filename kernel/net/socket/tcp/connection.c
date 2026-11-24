#include "internal.h"

#include "proc/accounting.h"
#include "lib/mem.h"
#include "net/net.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "proc/thread.h"
#include "lib/random.h"

enum {
  EINPROGRESS = 115,
  EOPNOTSUPP = 95,
  ECONNRESET = 104,
  TCP_PORT_BUCKETS = 32,
};

static struct open_file *tcp_port_buckets[TCP_PORT_BUCKETS];

static struct open_file **tcp_port_bucket(uint16_t port) {
  return &tcp_port_buckets[port % TCP_PORT_BUCKETS];
}

void tcp_set_local_port(struct open_file *file, uint16_t port) {
  if (file->tcp_local_port == port) { return; }
  file->tcp_local_port = port;
  socket_index_insert(tcp_port_bucket(port), file);
}

static int tcp_send_fin_once(struct open_file *file) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_error != 0) { return -(int)file->tcp_error; }
  if (file->tcp_fin_sent) { return 0; }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return -ENOTCONN; }
  uint8_t old_state = file->tcp_state;
  uint32_t seq = file->tcp_seq++;
  file->tcp_fin_sent = true;
  file->tcp_state = TCP_FIN_WAIT;
  if (!net_tcp_send_segment_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                    socket_effective_ttl(file), (uint8_t)file->ip_tos, seq, file->tcp_ack,
                                    tcp_window(file), TCP_FIN | TCP_ACK, NULL, 0)) {
    file->tcp_seq = seq;
    file->tcp_fin_sent = false;
    file->tcp_state = old_state;
    return -EIO;
  }
  return 0;
}

void tcp_send_abortive_close(struct open_file *file) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return; }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT && file->tcp_state != TCP_SYN_RECEIVED) {
    return;
  }
  (void)net_tcp_send_segment_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                     socket_effective_ttl(file), (uint8_t)file->ip_tos, file->tcp_seq, file->tcp_ack, 0,
                                     TCP_RST | TCP_ACK, NULL, 0);
  file->tcp_state = TCP_CLOSED;
}

static bool tcp_rst_acceptable(const struct open_file *file, uint32_t seq, uint32_t ack, uint8_t flags) {
  if (file == NULL) { return false; }
  if (file->tcp_state == TCP_SYN_SENT) { return (flags & TCP_ACK) != 0 && ack == file->tcp_seq; }
  if (file->tcp_state == TCP_SYN_RECEIVED) { return seq == file->tcp_ack; }
  if (file->tcp_state == TCP_ESTABLISHED || file->tcp_state == TCP_FIN_WAIT) { return seq == file->tcp_ack; }
  return false;
}

void tcp_ack_current_state(struct open_file *file) {
  if (file == NULL || (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT)) { return; }
  (void)net_tcp_send_segment_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                     socket_effective_ttl(file), (uint8_t)file->ip_tos, file->tcp_seq, file->tcp_ack,
                                     tcp_window(file), TCP_ACK, NULL, 0);
}

bool tcp_recv_waitall_pending(const struct open_file *file, uint64_t want, uint32_t flags) {
  if (!(flags & MSG_WAITALL) || file->tcp_fin || file->tcp_error) { return false; }
  uint32_t limit = socket_effective_buffer(file, true);
  return file->tcp_rx_len < want && file->tcp_rx_len < limit;
}

void tcp_init_socket(struct open_file *file) {
  tcp_init_rto(file);
  tcp_init_keepalive(file, cell_uptime_ticks());
}

int tcp_poll_events(const struct open_file *file, int events) {
  int ready = file->tcp_error ? CELL_POLLERR : 0;
  if (events & CELL_POLLIN) {
    net_poll();
    if (file->tcp_state == TCP_LISTEN) {
      if (file->tcp_ready_head) { ready |= CELL_POLLIN; }
    } else if (file->tcp_rx_len >= socket_effective_rcvlowat(file) || file->tcp_fin || file->tcp_error) {
      ready |= CELL_POLLIN;
    }
  }
  if ((events & CELL_POLLOUT) && (cell_tcp_socket_writable(file) || file->tcp_error)) { ready |= CELL_POLLOUT; }
  return ready;
}

void tcp_release_file(struct open_file *file) {
  if (file->tcp_state == TCP_LISTEN) { tcp_close_backlog(file); }
  if (file->so_linger_enabled && file->so_linger_seconds == 0) {
    tcp_send_abortive_close(file);
  } else {
    (void)tcp_send_fin_once(file);
  }
  tcp_tx_clear(file);
  tcp_set_timer_active(file, false);
  tcp_free_rx(file);
  tcp_free_ooo(file);
  socket_index_remove(tcp_port_bucket(file->tcp_local_port), file);
}

int tcp_copy_peer_addr_to_domain(struct domain *domain, int accepted_fd, uint64_t addr, uint64_t addrlen) {
  if (addr == 0) { return 0; }
  if (domain == NULL || accepted_fd < 0 || accepted_fd >= MAX_FDS || domain->fds[accepted_fd] == NULL || addrlen == 0) {
    return -EFAULT;
  }
  struct open_file *file = domain->fds[accepted_fd];
  if (file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -ENOTCONN; }
  uint32_t user_len = 0;
  if (!vmm_copy_from_user(cell_domain_as(domain), &user_len, addrlen, sizeof(user_len))) { return -EFAULT; }
  struct sockaddr_in_cell sa;
  kmemset(&sa, 0, sizeof(sa));
  sa.sin_family = 2;
  sa.sin_port = net_bswap16(file->tcp_remote_port);
  sa.sin_addr = file->tcp_remote_ip;
  size_t copy_len = user_len < sizeof(sa) ? user_len : sizeof(sa);
  if (copy_len != 0 && !vmm_copy_to_user(cell_domain_as(domain), addr, &sa, copy_len)) { return -EFAULT; }
  uint32_t actual_len = sizeof(sa);
  if (!vmm_copy_to_user(cell_domain_as(domain), addrlen, &actual_len, sizeof(actual_len))) { return -EFAULT; }
  return 0;
}

static struct open_file *tcp_socket_for_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || domain->fds[fd]->type != OPEN_SOCKET ||
      domain->fds[fd]->socket_proto != IPPROTO_TCP) {
    return NULL;
  }
  return domain->fds[fd];
}

uint32_t tcp_initial_seq(void) {
  uint32_t seq = 0;
  random_bytes(&seq, sizeof(seq));
  return seq;
}

static bool tcp_local_port_in_use(uint16_t port, const struct open_file *except) {
  for (struct open_file *file = *tcp_port_bucket(port); file != NULL; file = file->socket_index_next) {
    if (file != except && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
        file->tcp_state != TCP_CLOSED && file->tcp_local_port == port) {
      return true;
    }
  }
  return false;
}

static uint16_t tcp_ephemeral_port(void) {
  uint16_t seed = 0;
  random_bytes(&seed, sizeof(seed));
  uint16_t first = (uint16_t)(49152u + (seed % (65535u - 49152u)));
  uint16_t port = first;
  do {
    if (!tcp_local_port_in_use(port, NULL)) { return port; }
    port = port == 65534u ? 49152u : (uint16_t)(port + 1u);
  } while (port != first);
  return 0;
}

int cell_fd_tcp_bind(int fd, uint16_t port) {
  struct open_file *file = tcp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL) { return -9; }
  if (file->tcp_state != TCP_CLOSED && file->tcp_state != TCP_LISTEN) { return -EINVAL; }
  if (file->tcp_local_port != 0 && file->tcp_local_port != port) { return -EINVAL; }
  if (port == 0) {
    port = tcp_ephemeral_port();
    if (port == 0) { return -EADDRINUSE; }
  } else if (tcp_local_port_in_use(port, file)) {
    return -EADDRINUSE;
  }
  tcp_set_local_port(file, port);
  return 0;
}

int cell_fd_tcp_listen(int fd, int backlog) {
  struct open_file *file = tcp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL) { return -9; }
  if (file->tcp_state != TCP_CLOSED && file->tcp_state != TCP_LISTEN) { return -EINVAL; }
  if (file->tcp_local_port == 0) {
    uint16_t port = tcp_ephemeral_port();
    if (port == 0) { return -EADDRINUSE; }
    tcp_set_local_port(file, port);
  }
  file->tcp_listen_backlog = backlog <= 0 ? 1 : (uint16_t)(backlog > MAX_OPEN_FILES ? MAX_OPEN_FILES : backlog);
  file->tcp_state = TCP_LISTEN;
  file->tcp_error = 0;
  file->tcp_fin = false;
  file->tcp_fin_sent = false;
  file->tcp_fin_pending = false;
  file->tcp_retransmit_deadline_tick = 0;
  return 0;
}

int cell_fd_tcp_accept(int fd, uint64_t addr, uint64_t addrlen, int flags, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *listener = tcp_socket_for_fd(domain, fd);
  if (listener == NULL) { return -9; }
  if (listener->tcp_state != TCP_LISTEN) { return -EINVAL; }
  int accepted = cell_socket_take_pending_tcp(domain, listener, flags);
  if (accepted != -EAGAIN && accepted < 0) { return accepted; }
  if (accepted >= 0) {
    int rc = tcp_copy_peer_addr_to_domain(domain, accepted, addr, addrlen);
    if (rc < 0) {
      socket_close_domain_fd(domain, accepted);
      return rc;
    }
    return accepted;
  }
  if ((listener->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return -EAGAIN; }
  return cell_block_current_on_socket_accept(fd, addr, addrlen, flags, frame);
}

int cell_fd_tcp_connect(int fd, uint32_t ip, uint16_t port, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = tcp_socket_for_fd(domain, fd);
  if (file == NULL) { return -9; }
  if (file->tcp_state == TCP_ESTABLISHED) { return 0; }
  if (!cell_egress_allowed(IPPROTO_TCP, ip, port)) { return -EPERM; }
  if (file->tcp_state == TCP_CLOSED) {
    file->tcp_remote_ip = ip;
    file->tcp_remote_port = port;
    if (file->tcp_local_port == 0) {
      uint16_t local_port = tcp_ephemeral_port();
      if (local_port == 0) { return -EIO; }
      tcp_set_local_port(file, local_port);
    }
    file->tcp_seq = tcp_initial_seq();
    file->tcp_ack = 0;
    tcp_init_rto(file);
    file->tcp_error = 0;
    file->tcp_fin = false;
    file->tcp_fin_sent = false;
    file->tcp_fin_pending = false;
    file->tcp_fin_seq = 0;
    file->tcp_syn_retries = 0;
    file->tcp_retransmit_deadline_tick = 0;
    tcp_note_activity(file, cell_uptime_ticks());
    tcp_tx_clear(file);
    tcp_free_ooo(file);
    tcp_free_rx(file);
    tcp_set_timer_active(file, true);
    file->tcp_state = TCP_SYN_SENT;
    uint32_t syn_seq = file->tcp_seq;
    file->tcp_seq += 1;
    uint64_t now_ticks = cell_uptime_ticks();
    file->tcp_syn_sent_tick = now_ticks;
    file->tcp_retransmit_deadline_tick = now_ticks + tcp_current_rto(file);
    if (!tcp_send_syn(file, syn_seq, false)) {
      tcp_fail(file, EIO);
      return -EIO;
    }
  }
  net_poll();
  if (file->tcp_error != 0) { return -(int)file->tcp_error; }
  if (file->tcp_state == TCP_ESTABLISHED) { return 0; }
  if ((file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return -EINPROGRESS; }
  return cell_block_current(frame, (struct thread_wait){.reason = WAIT_SOCKET, .target = fd});
}

int64_t cell_fd_tcp_send(int fd, uint64_t buf, uint64_t len, bool dontwait, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = tcp_socket_for_fd(domain, fd);
  int64_t wrote = cell_socket_tcp_write_from_domain(domain, file, buf, len);
  if (wrote != -EAGAIN || file == NULL || dontwait || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) {
    return wrote;
  }
  if (!tcp_send_would_block(file)) { return wrote; }
  return cell_block_current_on_socket_write_timeout(fd, buf, len, file->so_sndtimeo_ticks, frame);
}

int cell_fd_socket_shutdown(int fd, int how) {
  if (how < 0 || how > 2) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -EOPNOTSUPP; }
  if (file->tcp_error != 0) { return -(int)file->tcp_error; }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return -ENOTCONN; }
  if (how == 0 || how == 2) {
    file->tcp_fin = true;
    file->tcp_fin_pending = false;
    tcp_free_rx(file);
    cell_socket_wake_file(file);
  }
  if (how == 1 || how == 2) { return tcp_send_fin_once(file); }
  return 0;
}

bool cell_net_deliver_tcp_error(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, int error) {
  for (struct open_file *file = *tcp_port_bucket(local_port); file != NULL;
       file = file->socket_index_next) {
    if (file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP || file->tcp_local_port != local_port ||
        file->tcp_remote_ip != remote_ip || file->tcp_remote_port != remote_port) {
      continue;
    }
    tcp_fail(file, error);
    return true;
  }
  return false;
}

static bool socket_matches_tcp(struct open_file *file, uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
         file->tcp_local_port == dst_port && file->tcp_remote_ip == src_ip && file->tcp_remote_port == src_port;
}

static struct open_file *tcp_listener_for_port(uint16_t port) {
  for (struct open_file *file = *tcp_port_bucket(port); file != NULL; file = file->socket_index_next) {
    if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP && file->tcp_state == TCP_LISTEN &&
        file->tcp_local_port == port) {
      return file;
    }
  }
  return NULL;
}

static void tcp_deliver_syn_to_listener(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq,
                                        uint16_t window, const void *options, size_t options_len) {
  struct open_file *listener = tcp_listener_for_port(dst_port);
  if (listener == NULL) {
    (void)net_tcp_send_segment(dst_port, src_ip, src_port, 0, seq + 1, 0, TCP_RST | TCP_ACK, NULL, 0);
    return;
  }
  tcp_queue_syn(listener, src_ip, src_port, seq, window, options, options_len);
}

static uint32_t tcp_segment_ack_delta(uint8_t flags, size_t len) {
  uint32_t delta = (uint32_t)len;
  if ((flags & TCP_SYN) != 0) { ++delta; }
  if ((flags & TCP_FIN) != 0) { ++delta; }
  return delta;
}

static void tcp_send_reset_for_unmatched(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq,
                                         uint32_t ack, uint8_t flags, size_t len) {
  if ((flags & TCP_RST) != 0) { return; }
  if ((flags & TCP_ACK) != 0) {
    (void)net_tcp_send_segment(dst_port, src_ip, src_port, ack, 0, 0, TCP_RST, NULL, 0);
    return;
  }
  (void)net_tcp_send_segment(dst_port, src_ip, src_port, 0, seq + tcp_segment_ack_delta(flags, len), 0,
                             TCP_RST | TCP_ACK, NULL, 0);
}

static void tcp_deliver_to_file(struct open_file *file, uint32_t seq, uint32_t ack, uint16_t window, uint8_t flags,
                                const void *options, size_t options_len, const void *payload, size_t len) {
  if (file->tcp_state == TCP_SYN_SENT && (flags & (TCP_SYN | TCP_ACK | TCP_RST)) == (TCP_SYN | TCP_ACK) &&
      ack == file->tcp_seq) {
    if (file->tcp_syn_retries == 0) { tcp_note_rtt_sample(file, file->tcp_syn_sent_tick, cell_uptime_ticks()); }
    tcp_apply_syn_options(file, window, options, options_len);
    file->tcp_ack = seq + 1;
    file->tcp_state = TCP_ESTABLISHED;
    file->tcp_syn_retries = 0;
    file->tcp_retransmit_deadline_tick = 0;
    file->tcp_syn_sent_tick = 0;
    tcp_note_activity(file, cell_uptime_ticks());
    tcp_ack_current_state(file);
    cell_socket_wake_file(file);
    return;
  }
  if (file->tcp_state == TCP_SYN_RECEIVED && flags == TCP_SYN && seq + 1 == file->tcp_ack) {
    (void)tcp_send_syn(file, file->tcp_seq - 1, true);
    return;
  }
  if (file->tcp_state == TCP_SYN_RECEIVED && (flags & (TCP_ACK | TCP_RST | TCP_SYN)) == TCP_ACK &&
      seq == file->tcp_ack && ack == file->tcp_seq) {
    if (!file->tcp_syn_retries) { tcp_note_rtt_sample(file, file->tcp_syn_sent_tick, cell_uptime_ticks()); }
    file->tcp_state = TCP_ESTABLISHED;
    file->tcp_retransmit_deadline_tick = 0;
    tcp_ready_pending(file);
  }
  if ((flags & TCP_RST) != 0) {
    if (!tcp_rst_acceptable(file, seq, ack, flags)) {
      tcp_ack_current_state(file);
      return;
    }
    tcp_fail(file, file->tcp_state == TCP_SYN_SENT ? ECONNREFUSED : ECONNRESET);
    return;
  }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return; }
  if (flags & TCP_SYN) {
    tcp_ack_current_state(file);
    return;
  }
  if ((flags & TCP_ACK) != 0 && (int32_t)(ack - file->tcp_seq) > 0) {
    tcp_ack_current_state(file);
    return;
  }
  tcp_note_activity(file, cell_uptime_ticks());
  file->tcp_remote_window = (uint32_t)window << file->tcp_remote_window_scale;
  if ((flags & TCP_ACK) != 0) { tcp_tx_ack(file, ack); }
  if (!len && flags == TCP_ACK && seq + 1 == file->tcp_ack) {
    tcp_ack_current_state(file);
    return;
  }
  if (len || (flags & TCP_FIN)) {
    tcp_receive_segment(file, seq, payload, (uint32_t)len, (flags & TCP_FIN) != 0);
    tcp_ack_current_state(file);
  }
  cell_socket_wake_file(file);
}

void cell_net_deliver_tcp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint16_t window, uint8_t flags, const void *options, size_t options_len, const void *payload,
                          size_t len) {
  for (struct open_file *file = *tcp_port_bucket(dst_port); file != NULL;
       file = file->socket_index_next) {
    if (!socket_matches_tcp(file, src_ip, src_port, dst_port)) { continue; }
    bool pending = file->tcp_listener != NULL;
    if (pending) { cell_retain_open_file(file); }
    tcp_deliver_to_file(file, seq, ack, window, flags, options, options_len, payload, len);
    if (pending) { cell_release_open_file(file); }
    return;
  }
  if ((flags & TCP_SYN) != 0 && (flags & TCP_ACK) == 0) {
    tcp_deliver_syn_to_listener(src_ip, src_port, dst_port, seq, window, options, options_len);
    return;
  }
  tcp_send_reset_for_unmatched(src_ip, src_port, dst_port, seq, ack, flags, len);
}
