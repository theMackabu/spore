#include "internal.h"
#include "mm/pool.h"
#include "net/net.h"
#include "proc/accounting.h"
#include "proc/epoll.h"

enum { MAX_DATA_RETRIES = 8, ETIMEDOUT = 110 };

struct tcp_segment {
  struct tcp_segment *next;
  uint64_t deadline, sent, first_sent;
  uint32_t seq;
  uint16_t len, offset;
  uint8_t retries;
  uint8_t data[CELL_TCP_TX_CAP];
};

static struct object_pool segments;

static void recycle(struct open_file *file, struct tcp_segment *seg) {
  if (!file->tcp_tx_spare) { file->tcp_tx_spare = seg; }
  else { pool_free(&segments, seg); }
}

static void drop_head(struct open_file *file) {
  struct tcp_segment *seg = file->tcp_tx_head;
  file->tcp_tx_bytes -= seg->len;
  file->tcp_tx_head = seg->next;
  if (!file->tcp_tx_head) { file->tcp_tx_tail = NULL; }
  --file->tcp_tx_count;
  recycle(file, seg);
}

void tcp_tx_clear(struct open_file *file) {
  while (file->tcp_tx_head) { drop_head(file); }
  if (file->tcp_tx_spare) { pool_free(&segments, file->tcp_tx_spare); }
  file->tcp_tx_spare = NULL;
}

void tcp_tx_ack(struct open_file *file, uint32_t ack) {
  uint64_t now = cell_uptime_ticks();
  bool ambiguous = false;
  while (file->tcp_tx_head) {
    struct tcp_segment *seg = file->tcp_tx_head;
    int32_t acknowledged = (int32_t)(ack - seg->seq);
    if (acknowledged <= 0) { break; }
    if (acknowledged < seg->len) {
      seg->seq = ack;
      seg->offset += (uint16_t)acknowledged;
      seg->len -= (uint16_t)acknowledged;
      file->tcp_tx_bytes -= (uint32_t)acknowledged;
      break;
    }
    if (seg->retries) { ambiguous = true; }
    if (!ambiguous) { tcp_note_rtt_sample(file, seg->sent, now); }
    drop_head(file);
  }
}

static uint32_t tcp_send_chunk_limit(const struct open_file *file) {
  if (!file || file->tcp_tx_count >= CELL_TCP_TX_QUEUE_CAP) { return 0; }
  uint32_t limit = socket_effective_buffer(file, false);
  if (limit > file->tcp_remote_window) { limit = file->tcp_remote_window; }
  if (file->tcp_tx_bytes >= limit) { return 0; }
  limit -= file->tcp_tx_bytes;
  if (file->tcp_remote_mss && limit > file->tcp_remote_mss) { limit = file->tcp_remote_mss; }
  return limit < CELL_TCP_TX_CAP ? limit : CELL_TCP_TX_CAP;
}

bool cell_tcp_socket_writable(const struct open_file *file) {
  return file && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
         file->tcp_state == TCP_ESTABLISHED && !file->tcp_error && !file->tcp_fin_sent && tcp_send_chunk_limit(file);
}

bool tcp_send_would_block(const struct open_file *file) {
  return file && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
         file->tcp_state == TCP_ESTABLISHED && !file->tcp_error && !file->tcp_fin_sent && !tcp_send_chunk_limit(file);
}

static bool send_segment(struct open_file *file, const struct tcp_segment *seg) {
  return net_tcp_send_segment_ttl_tos(file->net_ns, file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                      socket_effective_ttl(file), (uint8_t)file->ip_tos, seg->seq, file->tcp_ack,
                                      tcp_window(file), TCP_PSH | TCP_ACK, seg->data + seg->offset, seg->len);
}

static int64_t write_segment(struct open_file *file, uint64_t len, tcp_copy_payload copy, void *source) {
  if (!file || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_error) { return -(int)file->tcp_error; }
  if (file->tcp_fin_sent) { return -EPIPE; }
  if (file->tcp_state != TCP_ESTABLISHED) { return -ENOTCONN; }
  if (!len) { return 0; }
  uint32_t chunk = tcp_send_chunk_limit(file);
  if (!chunk) { return -EAGAIN; }
  if (chunk > len) { chunk = (uint32_t)len; }
  struct tcp_segment *seg = file->tcp_tx_spare;
  if (seg) { file->tcp_tx_spare = NULL; }
  else {
    if (!segments.limit && !pool_init(&segments, sizeof(*seg), SIZE_MAX)) { return -12; }
    seg = pool_alloc(&segments);
    if (!seg) { return -12; }
  }
  int error = copy(source, seg->data, chunk);
  if (error) {
    recycle(file, seg);
    return error;
  }
  seg->next = NULL;
  seg->first_sent = seg->sent = cell_uptime_ticks();
  seg->deadline = seg->sent + tcp_current_rto(file);
  seg->seq = file->tcp_seq;
  seg->len = chunk;
  seg->offset = seg->retries = 0;
  struct tcp_segment *prev = file->tcp_tx_tail;
  if (prev) { prev->next = seg; }
  else { file->tcp_tx_head = seg; }
  file->tcp_tx_tail = seg;
  ++file->tcp_tx_count;
  file->tcp_tx_bytes += chunk;
  file->tcp_seq += chunk;
  if (!send_segment(file, seg)) {
    file->tcp_seq -= chunk;
    file->tcp_tx_bytes -= chunk;
    --file->tcp_tx_count;
    file->tcp_tx_tail = prev;
    if (prev) { prev->next = NULL; }
    else { file->tcp_tx_head = NULL; }
    recycle(file, seg);
    return -EIO;
  }
  cell_epoll_file_changed(file);
  return chunk;
}

int64_t tcp_write_stream(struct open_file *file, uint64_t len, tcp_copy_payload copy, void *source) {
  uint64_t sent = 0;
  do {
    int64_t n = write_segment(file, len - sent, copy, source);
    if (n <= 0) { return sent ? (int64_t)sent : n; }
    sent += (uint64_t)n;
  } while (sent < len);
  return (int64_t)sent;
}

int tcp_tx_retransmit(struct open_file *file, uint64_t now) {
  struct tcp_segment *seg = file->tcp_tx_head;
  if (!seg) { return 0; }
  uint64_t timeout = ((uint64_t)file->tcp_user_timeout_ms + 9) / 10;
  if (timeout && now - seg->first_sent >= timeout) { return ETIMEDOUT; }
  if (now < seg->deadline) { return 0; }
  if (seg->retries == MAX_DATA_RETRIES) { return ETIMEDOUT; }
  ++seg->retries;
  seg->sent = now;
  seg->deadline = now + tcp_backoff_rto(file, seg->retries);
  return send_segment(file, seg) ? 0 : EIO;
}
