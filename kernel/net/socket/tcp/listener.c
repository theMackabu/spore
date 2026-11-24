#include "internal.h"
#include "proc/accounting.h"
#include "proc/domain.h"
#include "proc/epoll.h"
#include "proc/poll.h"

static void insert_half_open(struct open_file *listener, struct open_file *server) {
  server->tcp_pending_next = listener->tcp_half_open;
  server->tcp_pending_link = &listener->tcp_half_open;
  if (server->tcp_pending_next) { server->tcp_pending_next->tcp_pending_link = &server->tcp_pending_next; }
  listener->tcp_half_open = server;
}

static void unlink_pending(struct open_file *server) {
  struct open_file *listener = server->tcp_listener;
  *server->tcp_pending_link = server->tcp_pending_next;
  if (server->tcp_pending_next) {
    server->tcp_pending_next->tcp_pending_link = server->tcp_pending_link;
  } else if (listener->tcp_ready_tail == &server->tcp_pending_next) {
    listener->tcp_ready_tail = server->tcp_pending_link;
  }
  server->tcp_pending_next = NULL;
  server->tcp_pending_link = NULL;
}

static void detach(struct open_file *server) {
  unlink_pending(server);
  --server->tcp_listener->tcp_pending_count;
  cell_epoll_file_changed(server->tcp_listener);
  server->tcp_listener = NULL;
}

void tcp_ready_pending(struct open_file *server) {
  struct open_file *listener = server->tcp_listener;
  unlink_pending(server);
  if (!listener->tcp_ready_tail) { listener->tcp_ready_tail = &listener->tcp_ready_head; }
  server->tcp_pending_link = listener->tcp_ready_tail;
  *listener->tcp_ready_tail = server;
  listener->tcp_ready_tail = &server->tcp_pending_next;
  cell_epoll_file_activity(listener, CELL_POLLIN);
  cell_socket_wake_file(listener);
}

void tcp_discard_pending(struct open_file *server) {
  detach(server);
  cell_release_open_file(server);
}

void tcp_close_backlog(struct open_file *listener) {
  while (listener->tcp_half_open || listener->tcp_ready_head) {
    struct open_file *server = listener->tcp_half_open ? listener->tcp_half_open : listener->tcp_ready_head;
    detach(server);
    tcp_send_abortive_close(server);
    cell_release_open_file(server);
  }
}

int cell_socket_take_pending_tcp(struct domain *domain, struct open_file *listener, int flags) {
  struct open_file *server = listener->tcp_ready_head;
  if (!server) { return -EAGAIN; }
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return fd; }
  detach(server);
  server->flags |= flags & CELL_O_NONBLOCK;
  cell_assign_fd(domain, fd, server, flags & CELL_O_CLOEXEC);
  return fd;
}

void tcp_queue_syn(struct open_file *listener, uint32_t src_ip, uint16_t src_port, uint32_t seq, uint16_t window,
                   const void *options, size_t options_len) {
  if (listener->tcp_pending_count >= listener->tcp_listen_backlog) { return; }
  struct open_file *server = cell_alloc_open_file();
  if (server == NULL) { return; }
  server->type = OPEN_SOCKET;
  server->socket_proto = IPPROTO_TCP;
  server->so_rcvlowat = listener->so_rcvlowat;
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
  tcp_set_local_port(server, listener->tcp_local_port);
  server->tcp_remote_ip = src_ip;
  server->tcp_remote_port = src_port;
  server->tcp_seq = tcp_initial_seq();
  server->tcp_ack = seq + 1;
  tcp_apply_syn_options(server, window, options, options_len);
  tcp_init_rto(server);
  tcp_note_activity(server, cell_uptime_ticks());
  server->tcp_state = TCP_SYN_RECEIVED;
  uint32_t syn_seq = server->tcp_seq;
  server->tcp_seq += 1;
  server->tcp_listener = listener;
  ++listener->tcp_pending_count;
  insert_half_open(listener, server);
  tcp_set_timer_active(server, true);
  uint64_t now = cell_uptime_ticks();
  server->tcp_syn_sent_tick = now;
  server->tcp_retransmit_deadline_tick = now + tcp_current_rto(server);
  if (!tcp_send_syn(server, syn_seq, true)) { tcp_fail(server, EIO); }
}
