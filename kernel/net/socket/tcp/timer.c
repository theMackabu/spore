#include "internal.h"
#include "net/net.h"

enum {
  ETIMEDOUT = 110,
  TCP_SYN_MAX_RETRIES = 5,
  TCP_TICKS_PER_SECOND = 100,
  TCP_DEFAULT_KEEPIDLE_SEC = 7200,
  TCP_DEFAULT_KEEPINTVL_SEC = 75,
  TCP_DEFAULT_KEEPCNT = 9,
};

static struct open_file *timed_sockets;
static struct open_file *timer_cursor;

void tcp_set_timer_active(struct open_file *file, bool active) {
  if (active && !file->tcp_timer_link) {
    file->tcp_timer_next = timed_sockets;
    file->tcp_timer_link = &timed_sockets;
    if (timed_sockets) { timed_sockets->tcp_timer_link = &file->tcp_timer_next; }
    timed_sockets = file;
  } else if (!active && file->tcp_timer_link) {
    if (timer_cursor == file) { timer_cursor = file->tcp_timer_next; }
    *file->tcp_timer_link = file->tcp_timer_next;
    if (file->tcp_timer_next) { file->tcp_timer_next->tcp_timer_link = file->tcp_timer_link; }
    file->tcp_timer_link = NULL;
    file->tcp_timer_next = NULL;
  }
}

static uint64_t tcp_keepalive_ticks(uint32_t seconds) {
  return seconds == 0 ? 0 : (uint64_t)seconds * TCP_TICKS_PER_SECOND;
}

void tcp_note_activity(struct open_file *file, uint64_t now_ticks) {
  if (file == NULL) { return; }
  file->tcp_last_activity_tick = now_ticks;
  file->tcp_keepalive_deadline_tick = 0;
  file->tcp_keepalive_probes = 0;
}

void tcp_init_keepalive(struct open_file *file, uint64_t now_ticks) {
  if (file == NULL) { return; }
  file->tcp_keepidle = TCP_DEFAULT_KEEPIDLE_SEC;
  file->tcp_keepintvl = TCP_DEFAULT_KEEPINTVL_SEC;
  file->tcp_keepcnt = TCP_DEFAULT_KEEPCNT;
  tcp_note_activity(file, now_ticks);
}

void tcp_fail(struct open_file *file, int error) {
  file->tcp_error = error;
  file->tcp_state = TCP_CLOSED;
  file->tcp_retransmit_deadline_tick = 0;
  file->tcp_syn_sent_tick = 0;
  file->tcp_keepalive_deadline_tick = 0;
  file->tcp_keepalive_probes = 0;
  tcp_set_timer_active(file, false);
  tcp_tx_clear(file);
  if (file->tcp_listener) {
    tcp_discard_pending(file);
  } else {
    cell_socket_wake_file(file);
  }
}

static void keepalive(struct open_file *file, uint64_t now) {
  if (!file->so_keepalive || file->tcp_state != TCP_ESTABLISHED) { return; }
  uint64_t idle = tcp_keepalive_ticks(file->tcp_keepidle), interval = tcp_keepalive_ticks(file->tcp_keepintvl);
  if (!idle || !interval || !file->tcp_keepcnt) { return; }
  uint64_t due = file->tcp_keepalive_probes ? file->tcp_keepalive_deadline_tick : file->tcp_last_activity_tick + idle;
  if (now < due) { return; }
  if (file->tcp_keepalive_probes >= file->tcp_keepcnt) {
    tcp_fail(file, ETIMEDOUT);
    return;
  }
  ++file->tcp_keepalive_probes;
  file->tcp_keepalive_deadline_tick = now + interval;
  if (!net_tcp_send_segment_ttl_tos(file->net_ns, file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                    socket_effective_ttl(file), (uint8_t)file->ip_tos, file->tcp_seq - 1, file->tcp_ack,
                                    tcp_window(file), TCP_ACK, NULL, 0)) {
    tcp_fail(file, EIO);
  }
}

static void service_timer(struct open_file *file, uint64_t now) {
  if (file->tcp_state == TCP_SYN_SENT || file->tcp_state == TCP_SYN_RECEIVED) {
    if (now < file->tcp_retransmit_deadline_tick) { return; }
    if (file->tcp_syn_retries >= TCP_SYN_MAX_RETRIES) {
      tcp_fail(file, ETIMEDOUT);
      return;
    }
    ++file->tcp_syn_retries;
    file->tcp_syn_sent_tick = now;
    file->tcp_retransmit_deadline_tick = now + tcp_backoff_rto(file, file->tcp_syn_retries);
    if (!tcp_send_syn(file, file->tcp_seq - 1, file->tcp_state == TCP_SYN_RECEIVED)) { tcp_fail(file, EIO); }
    return;
  }
  if (file->tcp_tx_count) {
    int error = tcp_tx_retransmit(file, now);
    if (error) { tcp_fail(file, error); }
  } else {
    keepalive(file, now);
  }
}

void tcp_timer_tick(uint64_t now) {
  timer_cursor = timed_sockets;
  while (timer_cursor) {
    struct open_file *file = timer_cursor;
    timer_cursor = file->tcp_timer_next;
    service_timer(file, now);
  }
}
