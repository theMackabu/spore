#include "internal.h"

#include "proc/accounting.h"
#include "lib/mem.h"
#include "mm/pmm.h"
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
  TCP_OOO_BUFFER_SLOTS = 32,
  TCP_PORT_BUCKETS = 32,
  TCP_OOO_ENTRIES = 8,
  TCP_OOO_PAYLOAD_CAP = 1460,
  TCP_OOO_STATE_PAGES = (TCP_OOO_ENTRIES + 1) / 2,
};

struct tcp_ooo_state {
  bool used[TCP_OOO_ENTRIES];
  uint32_t seq[TCP_OOO_ENTRIES];
  uint32_t len[TCP_OOO_ENTRIES];
  uint16_t data_offset[TCP_OOO_ENTRIES];
  uint64_t pages[TCP_OOO_STATE_PAGES];
};

static_assert(2 * TCP_OOO_PAYLOAD_CAP <= PAGE_SIZE, "two TCP reorder segments fit in one page");

struct tcp_pending_conn {
  bool used;
  struct open_file *listener;
  struct open_file *server_end;
};

static struct tcp_pending_conn tcp_pending[16];

static struct open_file *tcp_port_buckets[TCP_PORT_BUCKETS];

static struct tcp_ooo_state tcp_ooo_buffers[TCP_OOO_BUFFER_SLOTS];

static bool tcp_ooo_used[TCP_OOO_BUFFER_SLOTS];

static struct open_file **tcp_port_bucket(uint16_t port) {
  return &tcp_port_buckets[port % TCP_PORT_BUCKETS];
}

static void tcp_set_local_port(struct open_file *file, uint16_t port) {
  if (file->tcp_local_port == port) { return; }
  file->tcp_local_port = port;
  socket_index_insert(tcp_port_bucket(port), file);
}

static bool tcp_alloc_ooo(struct open_file *file) {
  if (file == NULL) { return false; }
  if (file->tcp_ooo != NULL) { return true; }
  for (size_t i = 0; i < TCP_OOO_BUFFER_SLOTS; ++i) {
    if (tcp_ooo_used[i]) { continue; }
    tcp_ooo_used[i] = true;
    file->tcp_ooo_slot = (uint8_t)(i + 1);
    file->tcp_ooo = &tcp_ooo_buffers[i];
    kmemset(file->tcp_ooo, 0, sizeof(*file->tcp_ooo));
    return true;
  }
  return false;
}

static void tcp_free_ooo_pages(struct tcp_ooo_state *ooo) {
  if (ooo == NULL) { return; }
  for (size_t i = 0; i < TCP_OOO_STATE_PAGES; ++i) {
    if (ooo->pages[i] != 0) { pmm_free_page(ooo->pages[i]); }
  }
  kmemset(ooo->pages, 0, sizeof(ooo->pages));
}

static uint8_t *tcp_ooo_data(struct tcp_ooo_state *ooo, size_t slot, bool create) {
  if (ooo == NULL || slot >= TCP_OOO_ENTRIES) { return NULL; }
  size_t page_index = slot / 2;
  if (ooo->pages[page_index] == 0 && create) {
    ooo->pages[page_index] = pmm_alloc_zero_page();
  }
  if (ooo->pages[page_index] == 0) { return NULL; }
  uint8_t *page = pmm_phys_to_virt(ooo->pages[page_index]);
  return page == NULL ? NULL : page + (slot % 2) * TCP_OOO_PAYLOAD_CAP;
}

static void tcp_ooo_reclaim_slot(struct tcp_ooo_state *ooo, size_t slot) {
  if (ooo == NULL || slot >= TCP_OOO_ENTRIES) { return; }
  size_t page_index = slot / 2;
  size_t first = page_index * 2;
  if (ooo->pages[page_index] == 0 || ooo->used[first] || (first + 1 < TCP_OOO_ENTRIES && ooo->used[first + 1])) {
    return;
  }
  pmm_free_page(ooo->pages[page_index]);
  ooo->pages[page_index] = 0;
}

static void tcp_free_ooo(struct open_file *file) {
  if (file == NULL || file->tcp_ooo_slot == 0) { return; }
  size_t slot = (size_t)file->tcp_ooo_slot - 1;
  if (slot < TCP_OOO_BUFFER_SLOTS) {
    tcp_free_ooo_pages(&tcp_ooo_buffers[slot]);
    kmemset(&tcp_ooo_buffers[slot], 0, sizeof(tcp_ooo_buffers[slot]));
    tcp_ooo_used[slot] = false;
  }
  file->tcp_ooo_slot = 0;
  file->tcp_ooo = NULL;
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

static void tcp_send_abortive_close(struct open_file *file) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return; }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT && file->tcp_state != TCP_SYN_RECEIVED) {
    return;
  }
  (void)net_tcp_send_segment_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                     socket_effective_ttl(file), (uint8_t)file->ip_tos, file->tcp_seq, file->tcp_ack, 0,
                                     TCP_RST | TCP_ACK, NULL, 0);
  file->tcp_state = TCP_CLOSED;
}

static bool seq_before(uint32_t a, uint32_t b) {
  return (int32_t)(a - b) < 0;
}

static bool seq_after(uint32_t a, uint32_t b) {
  return (int32_t)(a - b) > 0;
}

static bool tcp_ack_after_sent(const struct open_file *file, uint32_t ack) {
  return file != NULL && seq_after(ack, file->tcp_seq);
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

static void tcp_clear_ooo(struct open_file *file) {
  if (file == NULL || file->tcp_ooo == NULL) { return; }
  tcp_free_ooo_pages(file->tcp_ooo);
  kmemset(file->tcp_ooo, 0, sizeof(*file->tcp_ooo));
}

static bool tcp_accept_pending_fin(struct open_file *file) {
  if (!file->tcp_fin_pending || file->tcp_fin_seq != file->tcp_ack) { return false; }
  file->tcp_fin_pending = false;
  file->tcp_fin = true;
  file->tcp_ack = file->tcp_fin_seq + 1;
  return true;
}

static void tcp_record_fin(struct open_file *file, uint32_t fin_seq) {
  if (seq_before(fin_seq, file->tcp_ack)) { return; }
  file->tcp_fin_pending = true;
  file->tcp_fin_seq = fin_seq;
}

static bool tcp_ooo_empty(const struct open_file *file);

static void tcp_store_ooo(struct open_file *file, uint32_t seq, const uint8_t *data, uint32_t len) {
  if (len == 0) { return; }
  uint32_t end = seq + len;
  if (!seq_after(end, file->tcp_ack)) { return; }
  if (seq_before(seq, file->tcp_ack)) {
    uint32_t skip = file->tcp_ack - seq;
    if (skip >= len) { return; }
    seq += skip;
    data += skip;
    len -= skip;
  }
  if (!tcp_alloc_ooo(file)) { return; }
  struct tcp_ooo_state *ooo = file->tcp_ooo;
  uint32_t n = len < TCP_OOO_PAYLOAD_CAP ? len : TCP_OOO_PAYLOAD_CAP;
  size_t slot = TCP_OOO_ENTRIES;
  for (size_t i = 0; i < TCP_OOO_ENTRIES; ++i) {
    if (!ooo->used[i]) {
      if (slot == TCP_OOO_ENTRIES) { slot = i; }
      continue;
    }
    if (ooo->seq[i] == seq) {
      if (ooo->len[i] >= n) { return; }
      slot = i;
      break;
    }
  }
  if (slot == TCP_OOO_ENTRIES) { return; }
  uint8_t *dst = tcp_ooo_data(ooo, slot, true);
  if (dst == NULL) {
    if (tcp_ooo_empty(file)) { tcp_free_ooo(file); }
    return;
  }
  kmemcpy(dst, data, n);
  ooo->seq[slot] = seq;
  ooo->len[slot] = n;
  ooo->data_offset[slot] = 0;
  ooo->used[slot] = true;
}

static int tcp_next_ooo_slot(const struct open_file *file) {
  if (file == NULL || file->tcp_ooo == NULL) { return -1; }
  const struct tcp_ooo_state *ooo = file->tcp_ooo;
  int best = -1;
  for (size_t i = 0; i < TCP_OOO_ENTRIES; ++i) {
    if (!ooo->used[i]) { continue; }
    if (best < 0 || seq_before(ooo->seq[i], ooo->seq[best])) { best = (int)i; }
  }
  return best;
}

static bool tcp_ooo_empty(const struct open_file *file) {
  if (file == NULL || file->tcp_ooo == NULL) { return true; }
  const struct tcp_ooo_state *ooo = file->tcp_ooo;
  for (size_t i = 0; i < TCP_OOO_ENTRIES; ++i) {
    if (ooo->used[i]) { return false; }
  }
  return true;
}

static void tcp_drain_ooo(struct open_file *file) {
  if (file == NULL || file->tcp_ooo == NULL) { return; }
  struct tcp_ooo_state *ooo = file->tcp_ooo;
  for (;;) {
    int slot = tcp_next_ooo_slot(file);
    if (slot < 0) {
      tcp_free_ooo(file);
      return;
    }
    uint32_t seq = ooo->seq[slot];
    uint32_t len = ooo->len[slot];
    if (seq_before(seq, file->tcp_ack)) {
      uint32_t skip = file->tcp_ack - seq;
      if (skip >= len) {
        ooo->used[slot] = false;
        ooo->len[slot] = 0;
        ooo->data_offset[slot] = 0;
        tcp_ooo_reclaim_slot(ooo, (size_t)slot);
        continue;
      }
      uint32_t remaining = len - skip;
      ooo->seq[slot] += skip;
      ooo->data_offset[slot] += (uint16_t)skip;
      ooo->len[slot] = remaining;
      seq = ooo->seq[slot];
      len = ooo->len[slot];
    }
    if (seq != file->tcp_ack) { return; }
    uint8_t *data = tcp_ooo_data(ooo, (size_t)slot, false);
    if (data == NULL) { return; }
    uint32_t n = tcp_append_rx(file, data + ooo->data_offset[slot], len);
    if (n == 0) { return; }
    (void)tcp_accept_pending_fin(file);
    if (n == len) {
      ooo->used[slot] = false;
      ooo->len[slot] = 0;
      ooo->data_offset[slot] = 0;
      tcp_ooo_reclaim_slot(ooo, (size_t)slot);
      continue;
    }
    uint32_t remaining = len - n;
    ooo->seq[slot] += n;
    ooo->data_offset[slot] += (uint16_t)n;
    ooo->len[slot] = remaining;
  }
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

static bool tcp_listener_readable(const struct open_file *listener) {
  for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
    if (tcp_pending[i].used && tcp_pending[i].listener == listener &&
        tcp_pending[i].server_end->tcp_state == TCP_ESTABLISHED) {
      return true;
    }
  }
  return false;
}

int tcp_poll_events(const struct open_file *file, int events) {
  int ready = file->tcp_error ? CELL_POLLERR : 0;
  if (events & CELL_POLLIN) {
    net_poll();
    if (file->tcp_state == TCP_LISTEN) {
      if (tcp_listener_readable(file)) { ready |= CELL_POLLIN; }
    } else if (file->tcp_rx_len >= socket_effective_rcvlowat(file) || file->tcp_fin || file->tcp_error) {
      ready |= CELL_POLLIN;
    }
  }
  if ((events & CELL_POLLOUT) && (cell_tcp_socket_writable(file) || file->tcp_error)) { ready |= CELL_POLLOUT; }
  return ready;
}

void tcp_release_file(struct open_file *file) {
  if (file->tcp_state == TCP_LISTEN) {
    for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
      if (tcp_pending[i].used && tcp_pending[i].listener == file) {
        cell_release_open_file(tcp_pending[i].server_end);
        tcp_pending[i] = (struct tcp_pending_conn){0};
      }
    }
  }
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

int cell_socket_take_pending_tcp(struct domain *domain, struct open_file *listener, int flags) {
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
    if (!tcp_pending[i].used || tcp_pending[i].listener != listener ||
        tcp_pending[i].server_end->tcp_state != TCP_ESTABLISHED) {
      continue;
    }
    domain->fds[fd] = tcp_pending[i].server_end;
    domain->fd_flags[fd] = 0;
    tcp_set_timer_active(tcp_pending[i].server_end, true);
    socket_index_insert(tcp_port_bucket(listener->tcp_local_port), tcp_pending[i].server_end);
    socket_apply_accept_flags(domain, fd, flags);
    tcp_pending[i] = (struct tcp_pending_conn){0};
    return fd;
  }
  return -EAGAIN;
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

static uint32_t tcp_initial_seq(struct domain *domain, int fd) {
  (void)domain;
  (void)fd;
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
  file->tcp_listen_backlog = backlog <= 0 ? 1 : (uint16_t)(backlog > 16 ? 16 : backlog);
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
    file->tcp_seq = tcp_initial_seq(domain, fd);
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
    tcp_clear_ooo(file);
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

static uint16_t tcp_pending_for_listener(const struct open_file *listener) {
  uint16_t count = 0;
  for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
    if (tcp_pending[i].used && tcp_pending[i].listener == listener) { ++count; }
  }
  return count;
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
  if (tcp_pending_for_listener(listener) >= listener->tcp_listen_backlog) { return; }

  size_t pending_slot = sizeof(tcp_pending) / sizeof(tcp_pending[0]);
  for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
    if (!tcp_pending[i].used) {
      pending_slot = i;
      break;
    }
  }
  if (pending_slot == sizeof(tcp_pending) / sizeof(tcp_pending[0])) { return; }

  struct open_file *server = cell_alloc_open_file();
  if (server == NULL) { return; }
  server->type = OPEN_SOCKET;
  server->socket_proto = IPPROTO_TCP;
  server->flags = listener->flags;
  server->so_reuseaddr = listener->so_reuseaddr;
  server->so_reuseport = listener->so_reuseport;
  server->so_keepalive = listener->so_keepalive;
  server->so_linger_enabled = listener->so_linger_enabled;
  server->tcp_nodelay = listener->tcp_nodelay;
  server->tcp_keepidle = listener->tcp_keepidle;
  server->tcp_keepintvl = listener->tcp_keepintvl;
  server->tcp_keepcnt = listener->tcp_keepcnt;
  server->tcp_user_timeout_ms = listener->tcp_user_timeout_ms;
  server->so_sndbuf = listener->so_sndbuf;
  server->so_rcvbuf = listener->so_rcvbuf;
  server->so_linger_seconds = listener->so_linger_seconds;
  server->so_rcvtimeo_ticks = listener->so_rcvtimeo_ticks;
  server->so_sndtimeo_ticks = listener->so_sndtimeo_ticks;
  server->ip_tos = listener->ip_tos;
  server->ip_ttl = listener->ip_ttl;
  server->ip_mtu_discover = listener->ip_mtu_discover;
  server->ip_bind_address_no_port = listener->ip_bind_address_no_port;
  server->tcp_local_port = dst_port;
  server->tcp_remote_ip = src_ip;
  server->tcp_remote_port = src_port;
  server->tcp_seq = tcp_initial_seq(NULL, 0);
  server->tcp_ack = seq + 1;
  tcp_apply_syn_options(server, window, options, options_len);
  tcp_init_rto(server);
  tcp_note_activity(server, cell_uptime_ticks());
  server->tcp_state = TCP_SYN_RECEIVED;
  uint32_t syn_seq = server->tcp_seq;
  server->tcp_seq += 1;
  tcp_pending[pending_slot] = (struct tcp_pending_conn){.used = true, .listener = listener, .server_end = server};
  if (!tcp_send_syn(server, syn_seq, true)) {
    cell_release_open_file(server);
    tcp_pending[pending_slot] = (struct tcp_pending_conn){0};
  }
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

static bool tcp_deliver_to_file(struct open_file *file, uint32_t seq, uint32_t ack, uint16_t window, uint8_t flags,
                                const void *options, size_t options_len, const void *payload, size_t len) {
  if (file->tcp_state == TCP_SYN_SENT && (flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == file->tcp_seq) {
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
    return true;
  }
  if (file->tcp_state == TCP_SYN_RECEIVED && (flags & TCP_ACK) != 0 && ack == file->tcp_seq) {
    file->tcp_remote_window = (uint32_t)window << file->tcp_remote_window_scale;
    file->tcp_state = TCP_ESTABLISHED;
    tcp_note_activity(file, cell_uptime_ticks());
    cell_socket_wake_file(file);
    for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
      if (tcp_pending[i].used && tcp_pending[i].server_end == file) {
        cell_socket_wake_file(tcp_pending[i].listener);
        break;
      }
    }
    return true;
  }
  if ((flags & TCP_RST) != 0) {
    if (!tcp_rst_acceptable(file, seq, ack, flags)) {
      tcp_ack_current_state(file);
      return true;
    }
    tcp_fail(file, file->tcp_state == TCP_SYN_SENT ? ECONNREFUSED : ECONNRESET);
    return true;
  }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return true; }
  if ((flags & TCP_ACK) != 0 && tcp_ack_after_sent(file, ack)) {
    tcp_ack_current_state(file);
    return true;
  }
  tcp_note_activity(file, cell_uptime_ticks());
  file->tcp_remote_window = (uint32_t)window << file->tcp_remote_window_scale;
  if ((flags & TCP_ACK) != 0) { tcp_tx_ack(file, ack); }
  if (!len && flags == TCP_ACK && seq + 1 == file->tcp_ack) {
    tcp_ack_current_state(file);
    return true;
  }
  uint32_t packet_fin_seq = seq + (uint32_t)len;
  if (len != 0) {
    const uint8_t *data = payload;
    uint32_t data_len = (uint32_t)len;
    if (seq_before(seq, file->tcp_ack)) {
      uint32_t skip = file->tcp_ack - seq;
      if (skip >= data_len) {
        tcp_ack_current_state(file);
        cell_socket_wake_file(file);
        return true;
      }
      data += skip;
      data_len -= skip;
      seq = file->tcp_ack;
    }
    if (seq != file->tcp_ack) {
      tcp_store_ooo(file, seq, data, data_len);
      if ((flags & TCP_FIN) != 0) { tcp_record_fin(file, packet_fin_seq); }
      tcp_ack_current_state(file);
      cell_socket_wake_file(file);
      return true;
    }
    uint32_t n = tcp_append_rx(file, data, data_len);
    if (n != 0) {
      tcp_drain_ooo(file);
      if ((flags & TCP_FIN) != 0) { tcp_record_fin(file, packet_fin_seq); }
      (void)tcp_accept_pending_fin(file);
      tcp_ack_current_state(file);
    } else {
      (void)net_tcp_send_segment_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                         socket_effective_ttl(file), (uint8_t)file->ip_tos, file->tcp_seq,
                                         file->tcp_ack, 0, TCP_ACK, NULL, 0);
    }
  }
  if ((flags & TCP_FIN) != 0) {
    tcp_record_fin(file, packet_fin_seq);
    if (tcp_accept_pending_fin(file)) { tcp_ack_current_state(file); }
  }
  cell_socket_wake_file(file);
  return true;
}

void cell_net_deliver_tcp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint16_t window, uint8_t flags, const void *options, size_t options_len, const void *payload,
                          size_t len) {
  for (struct open_file *file = *tcp_port_bucket(dst_port); file != NULL;
       file = file->socket_index_next) {
    if (!socket_matches_tcp(file, src_ip, src_port, dst_port)) { continue; }
    (void)tcp_deliver_to_file(file, seq, ack, window, flags, options, options_len, payload, len);
    return;
  }
  for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
    if (!tcp_pending[i].used || !socket_matches_tcp(tcp_pending[i].server_end, src_ip, src_port, dst_port)) {
      continue;
    }
    (void)tcp_deliver_to_file(tcp_pending[i].server_end, seq, ack, window, flags, options, options_len, payload, len);
    return;
  }
  if ((flags & TCP_SYN) != 0 && (flags & TCP_ACK) == 0) {
    tcp_deliver_syn_to_listener(src_ip, src_port, dst_port, seq, window, options, options_len);
    return;
  }
  tcp_send_reset_for_unmatched(src_ip, src_port, dst_port, seq, ack, flags, len);
}
