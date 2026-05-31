#include "proc/socket.h"

#include "kstr.h"
#include "mem.h"
#include "net.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/pipe.h"
#include "proc/poll.h"
#include "proc/thread.h"
#include "random.h"
#include "vfs.h"

enum {
  CELL_O_NONBLOCK = 04000,
  CELL_O_CLOEXEC = 02000000,
  SOCKET_MAX_IOVCNT = 1024,
  SOCKET_RECVMSG_SCRATCH_CAP = 1472,
  SOL_SOCKET = 1,
  SOCK_STREAM = 1,
  SOCK_DGRAM = 2,
  IPPROTO_IP = 0,
  IPPROTO_TCP = 6,
  IPPROTO_UDP = 17,
  IPPROTO_ICMP = 1,
  MSG_PEEK = 0x2,
  MSG_TRUNC = 0x20,
  MSG_WAITALL = 0x100,
  SO_REUSEADDR = 2,
  SO_DONTROUTE = 5,
  SO_BROADCAST = 6,
  SO_SNDBUF = 7,
  SO_RCVBUF = 8,
  SO_KEEPALIVE = 9,
  SO_REUSEPORT = 15,
  IP_TOS = 1,
  IP_MTU_DISCOVER = 10,
  IP_BIND_ADDRESS_NO_PORT = 24,
  TCP_NODELAY = 1,
  TCP_KEEPIDLE = 4,
  TCP_KEEPINTVL = 5,
  TCP_KEEPCNT = 6,
  TCP_FASTOPEN = 23,
  TCP_FASTOPEN_CONNECT = 30,
  EPERM = 1,
  EMSGSIZE = 90,
  EAGAIN = 11,
  EFAULT = 14,
  EACCES = 13,
  EINVAL = 22,
  EIO = 5,
  EINPROGRESS = 115,
  ENOPROTOOPT = 92,
  ENOTCONN = 107,
  EPIPE = 32,
  EOPNOTSUPP = 95,
  ECONNRESET = 104,
  ECONNREFUSED = 111,
  ETIMEDOUT = 110,
  EADDRINUSE = 98,
};

enum {
  DEFAULT_SOCKET_BUFFER = 262144,
};

enum tcp_socket_state {
  TCP_CLOSED,
  TCP_LISTEN,
  TCP_SYN_SENT,
  TCP_SYN_RECEIVED,
  TCP_ESTABLISHED,
  TCP_FIN_WAIT,
};

enum { TCP_RX_BUFFER_SLOTS = 16 };
enum { TCP_TX_BUFFER_SLOTS = 64 };
enum { DGRAM_RX_BUFFER_SLOTS = 64 };
enum { TCP_RCV_WINDOW_SCALE = 2 };
enum { TCP_DEFAULT_REMOTE_WINDOW = 65535 };
enum { TCP_DEFAULT_MSS = 1460, TCP_MIN_MSS = 536, TCP_SEND_STACK_CHUNK = 1400 };
enum {
  TCP_INITIAL_RTO_TICKS = 100,
  TCP_MIN_RTO_TICKS = 10,
  TCP_MAX_RTO_TICKS = 6000,
  TCP_MAX_RTO_SHIFT = 6,
  TCP_SYN_MAX_RETRIES = 5,
  TCP_DATA_MAX_RETRIES = 8,
};

struct unix_pending_conn {
  bool used;
  struct open_file *listener;
  struct open_file *server_end;
};

struct tcp_pending_conn {
  bool used;
  struct open_file *listener;
  struct open_file *server_end;
};

static struct unix_pending_conn unix_pending[16];
static struct tcp_pending_conn tcp_pending[16];
static uint8_t tcp_rx_buffers[TCP_RX_BUFFER_SLOTS][CELL_TCP_RX_CAP];
static bool tcp_rx_used[TCP_RX_BUFFER_SLOTS];
static uint8_t tcp_tx_buffers[TCP_TX_BUFFER_SLOTS][CELL_TCP_TX_CAP];
static bool tcp_tx_used[TCP_TX_BUFFER_SLOTS];
static uint8_t dgram_rx_buffers[DGRAM_RX_BUFFER_SLOTS][CELL_DGRAM_RX_CAP];
static bool dgram_rx_used[DGRAM_RX_BUFFER_SLOTS];
static uint16_t next_udp_port = 49152;

static int64_t cell_socket_tcp_write_from_kernel(struct open_file *file, const void *buf, uint64_t len);
static void socket_apply_accept_flags(struct domain *domain, int fd, int flags);

struct socket_iovec64 {
  uint64_t base;
  uint64_t len;
};

struct socket_msghdr64 {
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

enum { SOCKET_MSG_FLAGS_OFFSET = 48 };

struct sockaddr_in_cell {
  uint16_t sin_family;
  uint16_t sin_port;
  uint32_t sin_addr;
  uint8_t sin_zero[8];
};

static uint16_t net_bswap16(uint16_t v) {
  return (uint16_t)((v << 8) | (v >> 8));
}

static bool tcp_alloc_rx(struct open_file *file) {
  if (file == NULL) { return false; }
  if (file->tcp_rx != NULL) { return true; }
  for (size_t i = 0; i < TCP_RX_BUFFER_SLOTS; ++i) {
    if (tcp_rx_used[i]) { continue; }
    tcp_rx_used[i] = true;
    file->tcp_rx_slot = (uint8_t)(i + 1);
    file->tcp_rx = tcp_rx_buffers[i];
    file->tcp_rx_len = 0;
    return true;
  }
  return false;
}

static void tcp_free_rx(struct open_file *file) {
  if (file == NULL || file->tcp_rx_slot == 0) { return; }
  size_t slot = (size_t)file->tcp_rx_slot - 1;
  if (slot < TCP_RX_BUFFER_SLOTS) { tcp_rx_used[slot] = false; }
  file->tcp_rx_slot = 0;
  file->tcp_rx = NULL;
  file->tcp_rx_len = 0;
}

static int tcp_alloc_tx_slot(void) {
  for (size_t i = 0; i < TCP_TX_BUFFER_SLOTS; ++i) {
    if (tcp_tx_used[i]) { continue; }
    tcp_tx_used[i] = true;
    return (int)i;
  }
  return -1;
}

static void tcp_free_tx_slot(uint8_t slot) {
  if (slot < TCP_TX_BUFFER_SLOTS) { tcp_tx_used[slot] = false; }
}

static uint8_t tcp_tx_queue_index(const struct open_file *file, uint8_t offset) {
  return (uint8_t)((file->tcp_tx_head + offset) % CELL_TCP_TX_QUEUE_CAP);
}

static void tcp_tx_drop_head(struct open_file *file) {
  if (file == NULL || file->tcp_tx_count == 0) { return; }
  uint8_t index = file->tcp_tx_head;
  tcp_free_tx_slot(file->tcp_tx_slot[index]);
  file->tcp_tx_slot[index] = 0;
  file->tcp_tx_len[index] = 0;
  file->tcp_tx_seq[index] = 0;
  file->tcp_tx_retries[index] = 0;
  file->tcp_tx_deadline_tick[index] = 0;
  file->tcp_tx_sent_tick[index] = 0;
  file->tcp_tx_head = tcp_tx_queue_index(file, 1);
  --file->tcp_tx_count;
  if (file->tcp_tx_count == 0) { file->tcp_tx_head = 0; }
}

static void tcp_tx_drop_tail(struct open_file *file) {
  if (file == NULL || file->tcp_tx_count == 0) { return; }
  uint8_t index = tcp_tx_queue_index(file, (uint8_t)(file->tcp_tx_count - 1));
  tcp_free_tx_slot(file->tcp_tx_slot[index]);
  file->tcp_tx_slot[index] = 0;
  file->tcp_tx_len[index] = 0;
  file->tcp_tx_seq[index] = 0;
  file->tcp_tx_retries[index] = 0;
  file->tcp_tx_deadline_tick[index] = 0;
  file->tcp_tx_sent_tick[index] = 0;
  --file->tcp_tx_count;
  if (file->tcp_tx_count == 0) { file->tcp_tx_head = 0; }
}

static void tcp_tx_clear(struct open_file *file) {
  if (file == NULL) { return; }
  while (file->tcp_tx_count != 0) { tcp_tx_drop_head(file); }
}

static int dgram_alloc_rx_slot(void) {
  for (size_t i = 0; i < DGRAM_RX_BUFFER_SLOTS; ++i) {
    if (dgram_rx_used[i]) { continue; }
    dgram_rx_used[i] = true;
    return (int)i;
  }
  return -1;
}

static void dgram_free_rx_slot(uint8_t slot) {
  if (slot < DGRAM_RX_BUFFER_SLOTS) { dgram_rx_used[slot] = false; }
}

static uint8_t dgram_queue_index(const struct open_file *file, uint8_t offset) {
  return (uint8_t)((file->dgram_rx_head + offset) % CELL_DGRAM_RX_QUEUE_CAP);
}

static void dgram_drop_head(struct open_file *file) {
  if (file == NULL || file->dgram_rx_count == 0) { return; }
  uint8_t index = file->dgram_rx_head;
  dgram_free_rx_slot(file->dgram_rx_slot[index]);
  file->dgram_rx_slot[index] = 0;
  file->dgram_rx_len[index] = 0;
  file->dgram_rx_ip[index] = 0;
  file->dgram_rx_port[index] = 0;
  file->dgram_rx_head = dgram_queue_index(file, 1);
  --file->dgram_rx_count;
  if (file->dgram_rx_count == 0) { file->dgram_rx_head = 0; }
}

static void dgram_clear_rx(struct open_file *file) {
  if (file == NULL) { return; }
  while (file->dgram_rx_count != 0) { dgram_drop_head(file); }
}

static bool dgram_push_rx(struct open_file *file, uint32_t src_ip, uint16_t src_port, const void *payload, size_t len) {
  if (file == NULL || file->dgram_rx_count >= CELL_DGRAM_RX_QUEUE_CAP) { return false; }
  if (len > CELL_DGRAM_RX_CAP) { len = CELL_DGRAM_RX_CAP; }
  int slot = dgram_alloc_rx_slot();
  if (slot < 0) { return false; }
  uint8_t index = dgram_queue_index(file, file->dgram_rx_count);
  file->dgram_rx_slot[index] = (uint8_t)slot;
  file->dgram_rx_len[index] = (uint16_t)len;
  file->dgram_rx_ip[index] = src_ip;
  file->dgram_rx_port[index] = src_port;
  kmemcpy(dgram_rx_buffers[slot], payload, len);
  ++file->dgram_rx_count;
  return true;
}

static uint16_t tcp_window(const struct open_file *file) {
  uint32_t room = file->tcp_rx == NULL || file->tcp_rx_len >= CELL_TCP_RX_CAP ? 0 : CELL_TCP_RX_CAP - file->tcp_rx_len;
  uint32_t scaled = room >> TCP_RCV_WINDOW_SCALE;
  return scaled > UINT16_MAX ? UINT16_MAX : (uint16_t)scaled;
}

static uint16_t tcp_syn_window(const struct open_file *file) {
  uint32_t room = file->tcp_rx == NULL || file->tcp_rx_len >= CELL_TCP_RX_CAP ? 0 : CELL_TCP_RX_CAP - file->tcp_rx_len;
  return room > UINT16_MAX ? UINT16_MAX : (uint16_t)room;
}

static bool tcp_send_syn(struct open_file *file, uint32_t seq) {
  uint8_t options[8] = {
    2, 4, 0x05, 0xb4, // MSS 1460
    1, 3, 3, TCP_RCV_WINDOW_SCALE,
  };
  return net_tcp_send_segment_options(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, seq, 0,
                                      tcp_syn_window(file), 0x02, NULL, 0, options, sizeof(options));
}

static int tcp_send_fin_once(struct open_file *file) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_error != 0) { return -(int)file->tcp_error; }
  if (file->tcp_fin_sent) { return 0; }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return -ENOTCONN; }
  if (!net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                            file->tcp_ack, tcp_window(file), 0x11, NULL, 0)) {
    return -EIO;
  }
  file->tcp_fin_sent = true;
  file->tcp_state = TCP_FIN_WAIT;
  ++file->tcp_seq;
  return 0;
}

static uint32_t tcp_append_rx(struct open_file *file, const uint8_t *data, uint32_t len) {
  if (file->tcp_rx == NULL) { return 0; }
  uint32_t room = file->tcp_rx_len >= CELL_TCP_RX_CAP ? 0 : CELL_TCP_RX_CAP - file->tcp_rx_len;
  uint32_t n = len < room ? len : room;
  if (n == 0) { return 0; }
  kmemcpy(file->tcp_rx + file->tcp_rx_len, data, (size_t)n);
  file->tcp_rx_len += n;
  file->tcp_ack += n;
  return n;
}

static bool seq_before(uint32_t a, uint32_t b) {
  return (int32_t)(a - b) < 0;
}

static bool seq_after(uint32_t a, uint32_t b) {
  return (int32_t)(a - b) > 0;
}

static bool seq_before_or_equal(uint32_t a, uint32_t b) {
  return !seq_after(a, b);
}

static uint8_t tcp_parse_window_scale(const void *options, size_t len) {
  const uint8_t *opt = options;
  for (size_t i = 0; opt != NULL && i < len;) {
    uint8_t kind = opt[i];
    if (kind == 0) { break; }
    if (kind == 1) {
      ++i;
      continue;
    }
    if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len) { break; }
    if (kind == 3 && opt[i + 1] == 3) { return opt[i + 2] > 14 ? 14 : opt[i + 2]; }
    i += opt[i + 1];
  }
  return 0;
}

static uint16_t tcp_parse_mss(const void *options, size_t len) {
  const uint8_t *opt = options;
  for (size_t i = 0; opt != NULL && i < len;) {
    uint8_t kind = opt[i];
    if (kind == 0) { break; }
    if (kind == 1) {
      ++i;
      continue;
    }
    if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len) { break; }
    if (kind == 2 && opt[i + 1] == 4) {
      uint16_t mss = (uint16_t)(((uint16_t)opt[i + 2] << 8) | opt[i + 3]);
      if (mss < TCP_MIN_MSS) { return TCP_MIN_MSS; }
      return mss;
    }
    i += opt[i + 1];
  }
  return TCP_DEFAULT_MSS;
}

static uint32_t tcp_scaled_window(uint16_t window, uint8_t scale) {
  if (scale >= 16) { return UINT32_MAX; }
  return (uint32_t)window << scale;
}

static uint32_t tcp_clamp_rto(uint64_t ticks) {
  if (ticks < TCP_MIN_RTO_TICKS) { return TCP_MIN_RTO_TICKS; }
  if (ticks > TCP_MAX_RTO_TICKS) { return TCP_MAX_RTO_TICKS; }
  return (uint32_t)ticks;
}

static void tcp_init_rto(struct open_file *file) {
  if (file == NULL) { return; }
  file->tcp_srtt_ticks_x8 = 0;
  file->tcp_rttvar_ticks_x4 = 0;
  file->tcp_rto_ticks = TCP_INITIAL_RTO_TICKS;
  file->tcp_syn_sent_tick = 0;
}

static uint32_t tcp_current_rto(const struct open_file *file) {
  if (file == NULL || file->tcp_rto_ticks == 0) { return TCP_INITIAL_RTO_TICKS; }
  return tcp_clamp_rto(file->tcp_rto_ticks);
}

static uint64_t tcp_backoff_rto(const struct open_file *file, uint8_t retries) {
  uint32_t rto = tcp_current_rto(file);
  uint8_t shift = retries < TCP_MAX_RTO_SHIFT ? retries : TCP_MAX_RTO_SHIFT;
  if (rto > (TCP_MAX_RTO_TICKS >> shift)) { return TCP_MAX_RTO_TICKS; }
  return (uint64_t)rto << shift;
}

static void tcp_note_rtt_sample(struct open_file *file, uint64_t sent_tick, uint64_t now_ticks) {
  if (file == NULL || sent_tick == 0) { return; }
  uint64_t raw_sample = now_ticks > sent_tick ? now_ticks - sent_tick : 1;
  uint32_t sample = raw_sample > TCP_MAX_RTO_TICKS ? TCP_MAX_RTO_TICKS : (uint32_t)raw_sample;
  if (sample == 0) { sample = 1; }

  if (file->tcp_srtt_ticks_x8 == 0) {
    file->tcp_srtt_ticks_x8 = sample * 8u;
    file->tcp_rttvar_ticks_x4 = sample * 2u;
  } else {
    uint32_t srtt = file->tcp_srtt_ticks_x8 / 8u;
    uint32_t err = sample > srtt ? sample - srtt : srtt - sample;
    file->tcp_rttvar_ticks_x4 = (file->tcp_rttvar_ticks_x4 * 3u) / 4u + err;
    file->tcp_srtt_ticks_x8 = (file->tcp_srtt_ticks_x8 * 7u) / 8u + sample;
  }

  uint32_t srtt = file->tcp_srtt_ticks_x8 / 8u;
  uint32_t variance = file->tcp_rttvar_ticks_x4 == 0 ? 1u : file->tcp_rttvar_ticks_x4;
  file->tcp_rto_ticks = tcp_clamp_rto((uint64_t)srtt + variance);
}

static void tcp_update_remote_window(struct open_file *file, uint16_t window) {
  if (file == NULL) { return; }
  file->tcp_remote_window = tcp_scaled_window(window, file->tcp_remote_window_scale);
}

static uint32_t tcp_tx_inflight(const struct open_file *file) {
  if (file == NULL || file->tcp_tx_count == 0) { return 0; }
  return file->tcp_seq - file->tcp_tx_seq[file->tcp_tx_head];
}

static uint32_t tcp_send_window_available(const struct open_file *file) {
  if (file == NULL) { return 0; }
  uint32_t in_flight = tcp_tx_inflight(file);
  return file->tcp_remote_window > in_flight ? file->tcp_remote_window - in_flight : 0;
}

static uint32_t tcp_send_chunk_limit(const struct open_file *file) {
  uint32_t mss = file == NULL || file->tcp_remote_mss == 0 ? TCP_DEFAULT_MSS : file->tcp_remote_mss;
  return mss < TCP_SEND_STACK_CHUNK ? mss : TCP_SEND_STACK_CHUNK;
}

static bool tcp_tx_track_segment(struct open_file *file, uint32_t seq, const uint8_t *data, uint16_t len) {
  if (file == NULL || len == 0 || file->tcp_tx_count >= CELL_TCP_TX_QUEUE_CAP) { return false; }
  int slot = tcp_alloc_tx_slot();
  if (slot < 0) { return false; }
  uint8_t index = tcp_tx_queue_index(file, file->tcp_tx_count);
  file->tcp_tx_slot[index] = (uint8_t)slot;
  file->tcp_tx_len[index] = len;
  file->tcp_tx_seq[index] = seq;
  file->tcp_tx_retries[index] = 0;
  uint64_t now_ticks = cell_uptime_ticks();
  file->tcp_tx_sent_tick[index] = now_ticks;
  file->tcp_tx_deadline_tick[index] = now_ticks + tcp_current_rto(file);
  kmemcpy(tcp_tx_buffers[slot], data, len);
  ++file->tcp_tx_count;
  return true;
}

static void tcp_tx_ack(struct open_file *file, uint32_t ack) {
  uint64_t now_ticks = cell_uptime_ticks();
  bool ambiguous = false;
  while (file != NULL && file->tcp_tx_count != 0) {
    uint8_t index = file->tcp_tx_head;
    uint32_t end = file->tcp_tx_seq[index] + file->tcp_tx_len[index];
    if (!seq_before_or_equal(end, ack)) { return; }
    if (file->tcp_tx_retries[index] == 0 && !ambiguous) {
      tcp_note_rtt_sample(file, file->tcp_tx_sent_tick[index], now_ticks);
    } else {
      ambiguous = true;
    }
    tcp_tx_drop_head(file);
  }
}

static void tcp_clear_ooo(struct open_file *file) {
  for (size_t i = 0; i < sizeof(file->tcp_ooo_used) / sizeof(file->tcp_ooo_used[0]); ++i) {
    file->tcp_ooo_used[i] = false;
    file->tcp_ooo_len[i] = 0;
  }
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
  uint32_t n = len < sizeof(file->tcp_ooo[0]) ? len : (uint32_t)sizeof(file->tcp_ooo[0]);
  size_t slot = sizeof(file->tcp_ooo_used) / sizeof(file->tcp_ooo_used[0]);
  for (size_t i = 0; i < sizeof(file->tcp_ooo_used) / sizeof(file->tcp_ooo_used[0]); ++i) {
    if (!file->tcp_ooo_used[i]) {
      slot = i;
      break;
    }
    if (file->tcp_ooo_seq[i] == seq) {
      if (file->tcp_ooo_len[i] >= n) { return; }
      slot = i;
      break;
    }
  }
  if (slot == sizeof(file->tcp_ooo_used) / sizeof(file->tcp_ooo_used[0])) { return; }
  kmemcpy(file->tcp_ooo[slot], data, (size_t)n);
  file->tcp_ooo_seq[slot] = seq;
  file->tcp_ooo_len[slot] = n;
  file->tcp_ooo_used[slot] = true;
}

static int tcp_next_ooo_slot(const struct open_file *file) {
  int best = -1;
  for (size_t i = 0; i < sizeof(file->tcp_ooo_used) / sizeof(file->tcp_ooo_used[0]); ++i) {
    if (!file->tcp_ooo_used[i]) { continue; }
    if (best < 0 || seq_before(file->tcp_ooo_seq[i], file->tcp_ooo_seq[best])) { best = (int)i; }
  }
  return best;
}

static void tcp_drain_ooo(struct open_file *file) {
  for (;;) {
    int slot = tcp_next_ooo_slot(file);
    if (slot < 0) { return; }
    uint32_t seq = file->tcp_ooo_seq[slot];
    uint32_t len = file->tcp_ooo_len[slot];
    if (seq_before(seq, file->tcp_ack)) {
      uint32_t skip = file->tcp_ack - seq;
      if (skip >= len) {
        file->tcp_ooo_used[slot] = false;
        file->tcp_ooo_len[slot] = 0;
        continue;
      }
      uint32_t remaining = len - skip;
      for (uint32_t i = 0; i < remaining; ++i) {
        file->tcp_ooo[slot][i] = file->tcp_ooo[slot][skip + i];
      }
      file->tcp_ooo_seq[slot] += skip;
      file->tcp_ooo_len[slot] = remaining;
      seq = file->tcp_ooo_seq[slot];
      len = file->tcp_ooo_len[slot];
    }
    if (seq != file->tcp_ack) { return; }
    uint32_t n = tcp_append_rx(file, file->tcp_ooo[slot], len);
    if (n == 0) { return; }
    (void)tcp_accept_pending_fin(file);
    if (n == len) {
      file->tcp_ooo_used[slot] = false;
      file->tcp_ooo_len[slot] = 0;
      continue;
    }
    uint32_t remaining = len - n;
    for (uint32_t i = 0; i < remaining; ++i) {
      file->tcp_ooo[slot][i] = file->tcp_ooo[slot][n + i];
    }
    file->tcp_ooo_seq[slot] += n;
    file->tcp_ooo_len[slot] = remaining;
  }
}

int64_t cell_socket_tcp_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (domain == NULL || file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  uint8_t tmp[TCP_SEND_STACK_CHUNK];
  uint64_t limit = tcp_send_chunk_limit(file);
  uint64_t chunk = len > limit ? limit : len;
  if (chunk != 0 && !vmm_copy_from_user(cell_domain_as(domain), tmp, buf, (size_t)chunk)) { return -EFAULT; }
  return cell_socket_tcp_write_from_kernel(file, tmp, chunk);
}

static bool tcp_send_would_block(const struct open_file *file) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
         file->tcp_state == TCP_ESTABLISHED && file->tcp_error == 0 && !file->tcp_fin_sent &&
         (file->tcp_tx_count >= CELL_TCP_TX_QUEUE_CAP || tcp_send_window_available(file) == 0);
}

bool cell_tcp_socket_writable(const struct open_file *file) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
         file->tcp_state == TCP_ESTABLISHED && file->tcp_error == 0 && !file->tcp_fin_sent &&
         file->tcp_tx_count < CELL_TCP_TX_QUEUE_CAP && tcp_send_window_available(file) != 0;
}

static int64_t cell_socket_tcp_write_from_kernel(struct open_file *file, const void *buf, uint64_t len) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_error != 0) { return -(int64_t)file->tcp_error; }
  if (file->tcp_fin_sent) { return -EPIPE; }
  if (file->tcp_state != TCP_ESTABLISHED) { return -ENOTCONN; }
  if (len == 0) { return 0; }
  uint32_t available = tcp_send_window_available(file);
  if (available == 0) { return -EAGAIN; }
  uint64_t limit = tcp_send_chunk_limit(file);
  uint64_t chunk = len > limit ? limit : len;
  if (chunk > available) { chunk = available; }
  if (!tcp_tx_track_segment(file, file->tcp_seq, buf, (uint16_t)chunk)) { return -EAGAIN; }
  if (!net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                            file->tcp_ack, tcp_window(file), 0x18, buf, (size_t)chunk)) {
    tcp_tx_drop_tail(file);
    return -EIO;
  }
  file->tcp_seq += (uint32_t)chunk;
  return (int64_t)chunk;
}

static int64_t cell_socket_tcp_read_to_domain_flags(struct domain *domain, struct open_file *file, uint64_t buf,
                                                    uint64_t len, uint32_t flags) {
  if (file->tcp_rx_len == 0) {
    if (file->tcp_error != 0) { return -(int64_t)file->tcp_error; }
    return file->tcp_fin ? 0 : -EAGAIN;
  }
  if (file->tcp_rx == NULL) { return -EIO; }
  uint64_t n = file->tcp_rx_len < len ? file->tcp_rx_len : len;
  if (!vmm_copy_to_user(cell_domain_as(domain), buf, file->tcp_rx, (size_t)n)) { return -EFAULT; }
  if ((flags & MSG_PEEK) != 0) { return (int64_t)n; }
  if (n < file->tcp_rx_len) {
    uint32_t remaining = file->tcp_rx_len - (uint32_t)n;
    for (uint32_t i = 0; i < remaining; ++i) {
      file->tcp_rx[i] = file->tcp_rx[n + i];
    }
  }
  file->tcp_rx_len -= (uint32_t)n;
  if (file->tcp_state == TCP_ESTABLISHED || file->tcp_state == TCP_FIN_WAIT) {
    (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                               file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
  }
  return (int64_t)n;
}

int64_t cell_socket_tcp_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  return cell_socket_tcp_read_to_domain_flags(domain, file, buf, len, 0);
}

static int64_t cell_socket_tcp_read_to_kernel(struct open_file *file, void *buf, uint64_t len, uint32_t flags) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_rx_len == 0) {
    if (file->tcp_error != 0) { return -(int64_t)file->tcp_error; }
    return file->tcp_fin ? 0 : -EAGAIN;
  }
  if (file->tcp_rx == NULL) { return -EIO; }
  uint64_t n = file->tcp_rx_len < len ? file->tcp_rx_len : len;
  if (n != 0) { kmemcpy(buf, file->tcp_rx, (size_t)n); }
  if ((flags & MSG_PEEK) != 0) { return (int64_t)n; }
  if (n < file->tcp_rx_len) {
    uint32_t remaining = file->tcp_rx_len - (uint32_t)n;
    for (uint32_t i = 0; i < remaining; ++i) {
      file->tcp_rx[i] = file->tcp_rx[n + i];
    }
  }
  file->tcp_rx_len -= (uint32_t)n;
  if (file->tcp_state == TCP_ESTABLISHED || file->tcp_state == TCP_FIN_WAIT) {
    (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                               file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
  }
  return (int64_t)n;
}

static bool tcp_recv_waitall_pending(const struct open_file *file, uint64_t want, uint32_t flags) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
         (flags & MSG_WAITALL) != 0 && want != 0 && file->tcp_rx_len < want && file->tcp_error == 0 &&
         !file->tcp_fin;
}

int cell_fd_socket_inet(uint8_t proto) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int fd = -1;
  for (int i = 0; i < MAX_FDS; ++i) {
    if (domain->fds[i] == NULL) {
      fd = i;
      break;
    }
  }
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_SOCKET;
  file->socket_proto = proto;
  if (proto == IPPROTO_TCP) {
    file->tcp_remote_window = TCP_DEFAULT_REMOTE_WINDOW;
    file->tcp_remote_window_scale = 0;
    file->tcp_remote_mss = TCP_DEFAULT_MSS;
    tcp_init_rto(file);
  }
  if (proto == IPPROTO_TCP && !tcp_alloc_rx(file)) {
    cell_release_open_file(file);
    return -12;
  }
  domain->fds[fd] = file;
  domain->fd_flags[fd] = 0;
  return fd;
}

int cell_fd_socket_unix(void) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  struct open_file *file = cell_alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_UNIX_STREAM;
  file->unix_owner_pid = domain->id;
  file->unix_owner_uid = domain->euid;
  file->unix_owner_gid = domain->egid;
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
    case SO_SNDBUF:
      *out = file->so_sndbuf == 0 ? DEFAULT_SOCKET_BUFFER : (int32_t)file->so_sndbuf;
      return 0;
    case SO_RCVBUF:
      *out = file->so_rcvbuf == 0 ? DEFAULT_SOCKET_BUFFER : (int32_t)file->so_rcvbuf;
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
      file->so_sndbuf = (uint32_t)value;
      return 0;
    case SO_RCVBUF:
      if (value <= 0) { return -EINVAL; }
      file->so_rcvbuf = (uint32_t)value;
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
      if (value < 0) { return -EINVAL; }
      file->tcp_keepidle = (uint32_t)value;
      return 0;
    case TCP_KEEPINTVL:
      if (value < 0) { return -EINVAL; }
      file->tcp_keepintvl = (uint32_t)value;
      return 0;
    case TCP_KEEPCNT:
      if (value < 0) { return -EINVAL; }
      file->tcp_keepcnt = (uint32_t)value;
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
      file->ip_tos = (uint32_t)value;
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

bool cell_fd_socket_get_timeout(int fd, bool receive, uint64_t *ticks) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || ticks == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET && file->type != OPEN_UNIX_STREAM && file->type != OPEN_UNIX_LISTENER) {
    return false;
  }
  *ticks = receive ? file->so_rcvtimeo_ticks : file->so_sndtimeo_ticks;
  return true;
}

bool cell_fd_socket_set_timeout(int fd, bool receive, uint64_t ticks) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET && file->type != OPEN_UNIX_STREAM && file->type != OPEN_UNIX_LISTENER) {
    return false;
  }
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

static struct open_file *unix_file_for_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return NULL; }
  struct open_file *file = domain->fds[fd];
  return file->type == OPEN_UNIX_STREAM || file->type == OPEN_UNIX_LISTENER ? file : NULL;
}

static struct open_file *unix_listener_for_path(const char *path) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file != NULL && file->type == OPEN_UNIX_LISTENER && str_eq(file->unix_path, path)) { return file; }
    }
  }
  return NULL;
}

bool cell_unix_listener_readable(const struct open_file *listener) {
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (unix_pending[i].used && unix_pending[i].listener == listener) { return true; }
  }
  return false;
}

bool cell_tcp_listener_readable(const struct open_file *listener) {
  for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
    if (tcp_pending[i].used && tcp_pending[i].listener == listener &&
        tcp_pending[i].server_end->tcp_state == TCP_ESTABLISHED) {
      return true;
    }
  }
  return false;
}

void cell_socket_release_listener(struct open_file *file) {
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (unix_pending[i].used && unix_pending[i].listener == file) {
      cell_release_open_file(unix_pending[i].server_end);
      unix_pending[i] = (struct unix_pending_conn){0};
    }
  }
}

void cell_socket_release_file(struct open_file *file) {
  if (file == NULL || file->type != OPEN_SOCKET) { return; }
  if (file->socket_proto == IPPROTO_TCP) {
    if (file->tcp_state == TCP_LISTEN) {
      for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
        if (tcp_pending[i].used && tcp_pending[i].listener == file) {
          cell_release_open_file(tcp_pending[i].server_end);
          tcp_pending[i] = (struct tcp_pending_conn){0};
        }
      }
    }
    (void)tcp_send_fin_once(file);
    tcp_tx_clear(file);
    tcp_free_rx(file);
  }
  dgram_clear_rx(file);
}

int cell_fd_unix_bind(int fd, const char *path) {
  struct open_file *file = unix_file_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || file->type != OPEN_UNIX_STREAM || path == NULL || path[0] == '\0') { return -EINVAL; }
  struct vfs_node node;
  if (vfs_lookup(path, &node)) { return -EADDRINUSE; }
  if (!vfs_mksock(path, 0666, &node)) { return -EINVAL; }
  copy_cstr(file->unix_path, sizeof(file->unix_path), path);
  file->node = node;
  return 0;
}

int cell_fd_unix_listen(int fd, int backlog) {
  (void)backlog;
  struct open_file *file = unix_file_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || file->type != OPEN_UNIX_STREAM || file->unix_path[0] == '\0') { return -EINVAL; }
  file->type = OPEN_UNIX_LISTENER;
  return 0;
}

int cell_socket_take_pending_unix(struct domain *domain, struct open_file *listener, int flags) {
  int fd = cell_find_free_fd(domain, 0);
  if (fd < 0) { return -24; }
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (!unix_pending[i].used || unix_pending[i].listener != listener) { continue; }
    domain->fds[fd] = unix_pending[i].server_end;
    domain->fd_flags[fd] = 0;
    socket_apply_accept_flags(domain, fd, flags);
    unix_pending[i] = (struct unix_pending_conn){0};
    return fd;
  }
  return -EAGAIN;
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
    socket_apply_accept_flags(domain, fd, flags);
    tcp_pending[i] = (struct tcp_pending_conn){0};
    return fd;
  }
  return -EAGAIN;
}

static void socket_apply_accept_flags(struct domain *domain, int fd, int flags) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return; }
  if ((flags & CELL_O_NONBLOCK) != 0) { domain->fds[fd]->flags |= CELL_O_NONBLOCK; }
  if ((flags & CELL_O_CLOEXEC) != 0) { domain->fd_flags[fd] = 1; }
}

static void socket_close_domain_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return; }
  cell_release_open_file(domain->fds[fd]);
  domain->fds[fd] = NULL;
  domain->fd_flags[fd] = 0;
}

static int tcp_copy_peer_addr_to_domain(struct domain *domain, int accepted_fd, uint64_t addr, uint64_t addrlen) {
  if (addr == 0) { return 0; }
  if (domain == NULL || accepted_fd < 0 || accepted_fd >= MAX_FDS || domain->fds[accepted_fd] == NULL ||
      addrlen == 0) {
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

int cell_fd_unix_accept(int fd, int flags, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *listener = unix_file_for_fd(domain, fd);
  if (listener == NULL || listener->type != OPEN_UNIX_LISTENER) { return -9; }
  int accepted = cell_socket_take_pending_unix(domain, listener, flags);
  if (accepted != -EAGAIN) { return accepted; }
  if ((listener->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return -EAGAIN; }
  struct thread *thread = cell_current_thread_internal();
  if (thread != NULL) { thread->socket_accept_flags = flags; }
  return cell_block_current_on_socket(fd, 0, 0, 0, 0, frame);
}

int cell_fd_unix_connect(int fd, const char *path) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *client = unix_file_for_fd(domain, fd);
  if (client == NULL || client->type != OPEN_UNIX_STREAM || path == NULL) { return -9; }
  struct open_file *listener = unix_listener_for_path(path);
  if (listener == NULL) { return -ECONNREFUSED; }
  int c2s = cell_alloc_pipe_obj(0);
  int s2c = cell_alloc_pipe_obj(0);
  if (c2s < 0 || s2c < 0) {
    if (c2s >= 0) { cell_pipe_free((uint8_t)c2s); }
    if (s2c >= 0) { cell_pipe_free((uint8_t)s2c); }
    return -23;
  }
  struct open_file *server = cell_alloc_open_file();
  if (server == NULL) {
    cell_pipe_free((uint8_t)c2s);
    cell_pipe_free((uint8_t)s2c);
    return -12;
  }
  cell_pipe_add_reader((uint8_t)c2s);
  cell_pipe_add_writer((uint8_t)c2s);
  cell_pipe_add_reader((uint8_t)s2c);
  cell_pipe_add_writer((uint8_t)s2c);
  client->unix_rx_pipe = (uint8_t)s2c;
  client->unix_tx_pipe = (uint8_t)c2s;
  client->unix_peer_pid = listener->unix_owner_pid;
  client->unix_peer_uid = listener->unix_owner_uid;
  client->unix_peer_gid = listener->unix_owner_gid;
  copy_cstr(client->unix_path, sizeof(client->unix_path), path);
  server->type = OPEN_UNIX_STREAM;
  server->unix_rx_pipe = (uint8_t)c2s;
  server->unix_tx_pipe = (uint8_t)s2c;
  server->unix_owner_pid = listener->unix_owner_pid;
  server->unix_owner_uid = listener->unix_owner_uid;
  server->unix_owner_gid = listener->unix_owner_gid;
  server->unix_peer_pid = domain->id;
  server->unix_peer_uid = domain->euid;
  server->unix_peer_gid = domain->egid;
  server->node = listener->node;
  copy_cstr(server->unix_path, sizeof(server->unix_path), path);
  for (size_t i = 0; i < sizeof(unix_pending) / sizeof(unix_pending[0]); ++i) {
    if (!unix_pending[i].used) {
      unix_pending[i] = (struct unix_pending_conn){.used = true, .listener = listener, .server_end = server};
      cell_socket_wake_unix_accept_waiters(listener);
      return 0;
    }
  }
  cell_release_open_file(server);
  cell_pipe_free((uint8_t)c2s);
  cell_pipe_free((uint8_t)s2c);
  return -EAGAIN;
}

bool cell_fd_unix_peer_cred(int fd, struct cell_peer_cred *out) {
  struct open_file *file = unix_file_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || out == NULL || file->type != OPEN_UNIX_STREAM || file->unix_peer_pid <= 0) { return false; }
  out->pid = file->unix_peer_pid;
  out->uid = file->unix_peer_uid;
  out->gid = file->unix_peer_gid;
  return true;
}

static struct open_file *udp_socket_for_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || domain->fds[fd]->type != OPEN_SOCKET) {
    return NULL;
  }
  return domain->fds[fd];
}

static bool udp_local_port_in_use(uint16_t port, const struct open_file *except) {
  if (port == 0) { return false; }
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file != NULL && file != except && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_UDP &&
          file->udp_local_port == port) {
        return true;
      }
    }
  }
  return false;
}

static uint16_t udp_ephemeral_port(const struct open_file *file) {
  uint16_t first = next_udp_port;
  do {
    uint16_t port = next_udp_port++;
    if (next_udp_port < 49152) { next_udp_port = 49152; }
    if (!udp_local_port_in_use(port, file)) { return port; }
  } while (next_udp_port != first);
  return 0;
}

int cell_fd_udp_bind(int fd, uint16_t port) {
  struct open_file *file = udp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL) { return -9; }
  if (file->socket_proto != IPPROTO_UDP) { return -EINVAL; }
  if (file->udp_local_port != 0 && file->udp_local_port != port) { return -EINVAL; }
  if (port == 0) {
    port = udp_ephemeral_port(file);
    if (port == 0) { return -EADDRINUSE; }
  } else if (udp_local_port_in_use(port, file)) {
    return -EADDRINUSE;
  }
  file->udp_local_port = port;
  return 0;
}

bool cell_fd_udp_connect(int fd, uint32_t ip, uint16_t port) {
  struct open_file *file = udp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL || file->socket_proto != IPPROTO_UDP) { return false; }
  if (file->udp_local_port == 0) {
    uint16_t local_port = udp_ephemeral_port(file);
    if (local_port == 0) { return false; }
    file->udp_local_port = local_port;
  }
  file->udp_remote_ip = ip;
  file->udp_remote_port = port;
  file->udp_connected = true;
  file->udp_error = 0;
  return true;
}

int cell_fd_udp_disconnect(int fd) {
  struct open_file *file = udp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL) { return -9; }
  if (file->socket_proto != IPPROTO_UDP) { return -EINVAL; }
  file->udp_remote_ip = 0;
  file->udp_remote_port = 0;
  file->udp_connected = false;
  file->udp_error = 0;
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
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file != NULL && file != except && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
          file->tcp_state != TCP_CLOSED && file->tcp_local_port == port) {
        return true;
      }
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
  file->tcp_local_port = port;
  return 0;
}

int cell_fd_tcp_listen(int fd, int backlog) {
  struct open_file *file = tcp_socket_for_fd(cell_current_domain_internal(), fd);
  if (file == NULL) { return -9; }
  if (file->tcp_state != TCP_CLOSED && file->tcp_state != TCP_LISTEN) { return -EINVAL; }
  if (file->tcp_local_port == 0) {
    uint16_t port = tcp_ephemeral_port();
    if (port == 0) { return -EADDRINUSE; }
    file->tcp_local_port = port;
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
  struct thread *thread = cell_current_thread_internal();
  if (thread != NULL) { thread->socket_accept_flags = flags; }
  return cell_block_current_on_socket(fd, 0, 0, addr, addrlen, frame);
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
      file->tcp_local_port = tcp_ephemeral_port();
      if (file->tcp_local_port == 0) { return -EIO; }
    }
    file->tcp_seq = tcp_initial_seq(domain, fd);
    file->tcp_ack = 0;
    file->tcp_remote_window = TCP_DEFAULT_REMOTE_WINDOW;
    file->tcp_remote_window_scale = 0;
    file->tcp_remote_mss = TCP_DEFAULT_MSS;
    tcp_init_rto(file);
    file->tcp_error = 0;
    file->tcp_fin = false;
    file->tcp_fin_sent = false;
    file->tcp_fin_pending = false;
    file->tcp_fin_seq = 0;
    file->tcp_syn_retries = 0;
    file->tcp_retransmit_deadline_tick = 0;
    tcp_tx_clear(file);
    tcp_clear_ooo(file);
    file->tcp_state = TCP_SYN_SENT;
    uint32_t syn_seq = file->tcp_seq;
    file->tcp_seq += 1;
    if (!tcp_send_syn(file, syn_seq)) {
      file->tcp_state = TCP_CLOSED;
      return -EIO;
    }
    uint64_t now_ticks = cell_uptime_ticks();
    file->tcp_syn_sent_tick = now_ticks;
    file->tcp_retransmit_deadline_tick = now_ticks + tcp_current_rto(file);
  }
  net_poll();
  if (file->tcp_error != 0) { return -(int)file->tcp_error; }
  if (file->tcp_state == TCP_ESTABLISHED) { return 0; }
  if ((file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return -EINPROGRESS; }
  return cell_block_current_on_socket(fd, 0, 0, 0, 0, frame);
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

int64_t cell_fd_tcp_send_kernel(int fd, const void *buf, uint64_t len) {
  struct domain *domain = cell_current_domain_internal();
  return cell_socket_tcp_write_from_kernel(tcp_socket_for_fd(domain, fd), buf, len);
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
    file->tcp_rx_len = 0;
    cell_socket_wake_file(file);
  }
  if (how == 1 || how == 2) { return tcp_send_fin_once(file); }
  return 0;
}

static int64_t udp_send_payload(struct open_file *file, uint32_t ip, uint16_t port, const void *buf, uint64_t len) {
  if (file == NULL) { return -9; }
  if (buf == NULL && len != 0) { return -EFAULT; }
  uint32_t effective_ip = ip;
  uint16_t effective_port = port;
  if (effective_ip == 0 && effective_port == 0 && file->udp_connected) {
    effective_ip = file->udp_remote_ip;
    effective_port = file->udp_remote_port;
  }
  if (effective_ip == 0 || (file->socket_proto == IPPROTO_UDP && effective_port == 0)) { return -EINVAL; }
  if (file->socket_proto == IPPROTO_UDP && net_is_broadcast_addr(effective_ip) && !file->so_broadcast) {
    return -EACCES;
  }
  if (file->socket_proto == IPPROTO_UDP && !cell_egress_allowed(IPPROTO_UDP, effective_ip, effective_port)) {
    return -EPERM;
  }
  if (file->socket_proto == IPPROTO_UDP && file->udp_local_port == 0) {
    uint16_t local_port = udp_ephemeral_port(file);
    if (local_port == 0) { return -EADDRINUSE; }
    file->udp_local_port = local_port;
  }
  if (len > CELL_DGRAM_RX_CAP) { return -EMSGSIZE; }
  bool sent = false;
  if (file->socket_proto == IPPROTO_UDP) {
    sent = net_udp_send(file->udp_local_port, effective_ip, effective_port, buf, (size_t)len);
  } else if (file->socket_proto == IPPROTO_ICMP) {
    sent = net_icmp_send_echo(effective_ip, buf, (size_t)len);
  }
  if (!sent) {
    if (file->socket_proto == IPPROTO_UDP && net_is_loopback_addr(effective_ip)) {
      if (file->udp_connected && file->udp_remote_ip == effective_ip && file->udp_remote_port == effective_port) {
        file->udp_error = ECONNREFUSED;
        cell_socket_wake_file(file);
      }
      return (int64_t)len;
    }
    if (file->socket_proto == IPPROTO_UDP && !cell_egress_allowed(IPPROTO_UDP, effective_ip, effective_port)) {
      return -EPERM;
    }
    return -EIO;
  }
  return (int64_t)len;
}

int64_t cell_fd_udp_send(int fd, uint32_t ip, uint16_t port, uint64_t buf, uint64_t len) {
  struct domain *domain = cell_current_domain_internal();
  struct open_file *file = udp_socket_for_fd(domain, fd);
  if (file == NULL) { return -9; }
  uint8_t tmp[1472];
  if (len > sizeof(tmp)) { return -EMSGSIZE; }
  if (!vmm_copy_from_user(cell_domain_as(domain), tmp, buf, (size_t)len)) { return -EFAULT; }
  return udp_send_payload(file, ip, port, tmp, len);
}

int64_t cell_fd_udp_send_kernel(int fd, uint32_t ip, uint16_t port, const void *buf, uint64_t len) {
  return udp_send_payload(udp_socket_for_fd(cell_current_domain_internal(), fd), ip, port, buf, len);
}

static bool dgram_copy_source_to_domain(struct domain *domain, uint32_t src_ip, uint16_t src_port, uint64_t addr,
                                        uint64_t addrlen) {
  if (addr == 0 && addrlen == 0) { return true; }
  if (addrlen != 0) {
    uint32_t len = sizeof(struct sockaddr_in_cell);
    if (!vmm_copy_to_user(cell_domain_as(domain), addrlen, &len, sizeof(len))) { return false; }
  }
  if (addr == 0) { return true; }
  struct sockaddr_in_cell sa;
  kmemset(&sa, 0, sizeof(sa));
  sa.sin_family = 2;
  sa.sin_port = net_bswap16(src_port);
  sa.sin_addr = src_ip;
  return vmm_copy_to_user(cell_domain_as(domain), addr, &sa, sizeof(sa));
}

bool cell_socket_copy_udp_source_to_domain(struct domain *domain, const struct open_file *file, uint64_t addr,
                                           uint64_t addrlen) {
  if (file == NULL || file->dgram_rx_count == 0) { return dgram_copy_source_to_domain(domain, 0, 0, addr, addrlen); }
  uint8_t index = file->dgram_rx_head;
  return dgram_copy_source_to_domain(domain, file->dgram_rx_ip[index], file->dgram_rx_port[index], addr, addrlen);
}

static int64_t dgram_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len,
                                    uint64_t addr, uint64_t addrlen, uint32_t flags) {
  if (file != NULL && file->udp_error != 0) {
    int err = file->udp_error;
    file->udp_error = 0;
    return -(int64_t)err;
  }
  if (file == NULL || file->dgram_rx_count == 0) { return -EAGAIN; }
  uint8_t index = file->dgram_rx_head;
  uint8_t slot = file->dgram_rx_slot[index];
  uint64_t n = file->dgram_rx_len[index] < len ? file->dgram_rx_len[index] : len;
  if (!vmm_copy_to_user(cell_domain_as(domain), buf, dgram_rx_buffers[slot], (size_t)n)) { return -EFAULT; }
  if (!dgram_copy_source_to_domain(domain, file->dgram_rx_ip[index], file->dgram_rx_port[index], addr, addrlen)) {
    return -EFAULT;
  }
  if ((flags & MSG_PEEK) == 0) { dgram_drop_head(file); }
  return (int64_t)n;
}

static int64_t dgram_read_to_kernel(struct domain *domain, struct open_file *file, void *buf, uint64_t len,
                                    uint64_t addr, uint64_t addrlen, uint32_t flags, uint16_t *original_len) {
  if (file != NULL && file->udp_error != 0) {
    int err = file->udp_error;
    file->udp_error = 0;
    return -(int64_t)err;
  }
  if (file == NULL || file->dgram_rx_count == 0) { return -EAGAIN; }
  uint8_t index = file->dgram_rx_head;
  uint8_t slot = file->dgram_rx_slot[index];
  uint16_t packet_len = file->dgram_rx_len[index];
  if (original_len != NULL) { *original_len = packet_len; }
  uint64_t n = packet_len < len ? packet_len : len;
  if (n != 0) { kmemcpy(buf, dgram_rx_buffers[slot], (size_t)n); }
  if (!dgram_copy_source_to_domain(domain, file->dgram_rx_ip[index], file->dgram_rx_port[index], addr, addrlen)) {
    return -EFAULT;
  }
  if ((flags & MSG_PEEK) == 0) { dgram_drop_head(file); }
  return (int64_t)n;
}

int64_t cell_fd_socket_recv(int fd, uint64_t buf, uint64_t len, uint32_t flags, bool dontwait, struct trap_frame *frame,
                            uint64_t addr, uint64_t addrlen) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_UNIX_STREAM) {
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

static int socket_copy_iov_at(struct domain *domain, uint64_t iov_addr, int32_t iovlen, int32_t index,
                              struct socket_iovec64 *out) {
  if (domain == NULL || out == NULL || iovlen < 0 || iovlen > SOCKET_MAX_IOVCNT || index < 0 || index >= iovlen) {
    return -EINVAL;
  }
  uint64_t addr = iov_addr + (uint64_t)index * sizeof(*out);
  return vmm_copy_from_user(cell_domain_as(domain), out, addr, sizeof(*out)) ? 0 : -EFAULT;
}

static int socket_iovecs_capacity(struct domain *domain, uint64_t iov_addr, int32_t iovlen, uint64_t *capacity) {
  if (domain == NULL || capacity == NULL || iovlen < 0 || iovlen > SOCKET_MAX_IOVCNT) { return -EINVAL; }
  if (iovlen > 0 && iov_addr == 0) { return -EFAULT; }
  *capacity = 0;
  for (int32_t i = 0; i < iovlen; ++i) {
    struct socket_iovec64 iov;
    int rc = socket_copy_iov_at(domain, iov_addr, iovlen, i, &iov);
    if (rc < 0) { return rc; }
    if (UINT64_MAX - *capacity < iov.len) { return -EINVAL; }
    *capacity += iov.len;
  }
  return 0;
}

static int socket_gather_iovecs(struct domain *domain, uint64_t iov_addr, int32_t iovlen, uint8_t *dst, size_t cap,
                                uint64_t *total_len, size_t *copied) {
  if (domain == NULL || dst == NULL || total_len == NULL || copied == NULL || iovlen < 0 ||
      iovlen > SOCKET_MAX_IOVCNT) {
    return -EINVAL;
  }
  if (iovlen > 0 && iov_addr == 0) { return -EFAULT; }
  *total_len = 0;
  *copied = 0;
  for (int32_t i = 0; i < iovlen; ++i) {
    struct socket_iovec64 iov;
    int rc = socket_copy_iov_at(domain, iov_addr, iovlen, i, &iov);
    if (rc < 0) { return rc; }
    if (UINT64_MAX - *total_len < iov.len) { return -EINVAL; }
    *total_len += iov.len;
    if (*copied >= cap || iov.len == 0) { continue; }
    uint64_t room = cap - *copied;
    size_t n = (size_t)(iov.len < room ? iov.len : room);
    if (!vmm_copy_from_user(cell_domain_as(domain), dst + *copied, iov.base, n)) { return -EFAULT; }
    *copied += n;
  }
  return 0;
}

static int socket_scatter_iovecs(struct domain *domain, uint64_t iov_addr, int32_t iovlen, const uint8_t *src,
                                 uint64_t len) {
  uint64_t done = 0;
  for (int32_t i = 0; i < iovlen && done < len; ++i) {
    struct socket_iovec64 iov;
    int rc = socket_copy_iov_at(domain, iov_addr, iovlen, i, &iov);
    if (rc < 0) { return rc; }
    uint64_t remaining = len - done;
    size_t n = (size_t)(iov.len < remaining ? iov.len : remaining);
    if (n != 0 && !vmm_copy_to_user(cell_domain_as(domain), iov.base, src + done, n)) { return -EFAULT; }
    done += n;
  }
  return 0;
}

static int64_t socket_recvmsg_to_iov(struct domain *domain, struct open_file *file, uint64_t msg_addr, uint64_t iov,
                                     int32_t iovlen, uint64_t addr, uint64_t addrlen, uint32_t recv_flags) {
  uint64_t capacity = 0;
  int rc = socket_iovecs_capacity(domain, iov, iovlen, &capacity);
  if (rc < 0) { return rc; }
  if (capacity == 0) { return 0; }
  uint8_t tmp[SOCKET_RECVMSG_SCRATCH_CAP];
  uint64_t want = capacity < sizeof(tmp) ? capacity : sizeof(tmp);
  int64_t got = -EAGAIN;
  int32_t msg_flags = 0;
  if (file->socket_proto == IPPROTO_TCP) {
    if (tcp_recv_waitall_pending(file, want, recv_flags)) { return -EAGAIN; }
    got = cell_socket_tcp_read_to_kernel(file, tmp, want, recv_flags);
  } else if (file->socket_proto == IPPROTO_UDP && (file->dgram_rx_count != 0 || file->udp_error != 0)) {
    uint16_t packet_len = 0;
    got = dgram_read_to_kernel(domain, file, tmp, want, addr, addrlen, recv_flags, &packet_len);
    if (got >= 0 && packet_len > capacity) {
      msg_flags |= MSG_TRUNC;
      if ((recv_flags & MSG_TRUNC) != 0) { got = packet_len; }
    }
  }
  if (got < 0) { return got; }
  uint64_t scatter_len = (uint64_t)got < capacity ? (uint64_t)got : capacity;
  rc = socket_scatter_iovecs(domain, iov, iovlen, tmp, scatter_len);
  if (rc < 0) { return rc; }
  if (msg_addr != 0 && !vmm_copy_to_user(cell_domain_as(domain), msg_addr + SOCKET_MSG_FLAGS_OFFSET, &msg_flags,
                                         sizeof(msg_flags))) {
    return -EFAULT;
  }
  return got;
}

static int64_t socket_sendmsg_tcp_from_domain(struct domain *domain, struct open_file *file, uint64_t msg_addr) {
  if (domain == NULL || file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  struct socket_msghdr64 msg;
  if (!vmm_copy_from_user(cell_domain_as(domain), &msg, msg_addr, sizeof(msg))) { return -EFAULT; }
  uint8_t tmp[CELL_TCP_TX_CAP];
  uint64_t total_len = 0;
  size_t copied = 0;
  int rc = socket_gather_iovecs(domain, msg.iov, msg.iovlen, tmp, sizeof(tmp), &total_len, &copied);
  if (rc < 0) { return rc; }
  if (total_len == 0) { return 0; }
  return cell_socket_tcp_write_from_kernel(file, tmp, copied);
}

int64_t cell_fd_socket_sendmsg(int fd, uint64_t msg_addr, bool dontwait, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  net_poll();
  int64_t sent = socket_sendmsg_tcp_from_domain(domain, file, msg_addr);
  if (sent != -EAGAIN || dontwait || (file->flags & CELL_O_NONBLOCK) != 0 || frame == NULL) { return sent; }
  if (!tcp_send_would_block(file)) { return sent; }
  return cell_block_current_on_socket_sendmsg_timeout(fd, msg_addr, file->so_sndtimeo_ticks, frame);
}

int64_t cell_fd_socket_recvmsg(int fd, uint64_t msg_addr, uint32_t flags, bool dontwait, struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_SOCKET) { return -9; }
  struct socket_msghdr64 msg;
  if (!vmm_copy_from_user(cell_domain_as(domain), &msg, msg_addr, sizeof(msg))) { return -EFAULT; }
  net_poll();
  bool can_block = !dontwait && (file->flags & CELL_O_NONBLOCK) == 0 && frame != NULL;
  uint32_t recv_flags = can_block ? flags : (flags & ~((uint32_t)MSG_WAITALL));
  int64_t got = socket_recvmsg_to_iov(domain, file, msg_addr, msg.iov, msg.iovlen, msg.name, msg_addr + 8, recv_flags);
  if (got != -EAGAIN || !can_block) { return got; }
  return cell_block_current_on_socket_msg_timeout(fd, msg_addr, msg.iov, (uint64_t)msg.iovlen, msg.name,
                                                 msg_addr + 8, flags, file->so_rcvtimeo_ticks, frame);
}

static bool socket_matches_udp(struct open_file *file, uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_UDP &&
         file->udp_local_port == dst_port &&
         (!file->udp_connected || (file->udp_remote_ip == src_ip && file->udp_remote_port == src_port));
}

bool cell_net_deliver_udp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const void *payload, size_t len) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (!socket_matches_udp(file, src_ip, src_port, dst_port)) { continue; }
      if (dgram_push_rx(file, src_ip, src_port, payload, len)) { cell_socket_wake_file(file); }
      return true;
    }
  }
  return false;
}

bool cell_net_deliver_udp_error(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, int error) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_UDP || !file->udp_connected) {
        continue;
      }
      if (file->udp_local_port != local_port || file->udp_remote_ip != remote_ip ||
          file->udp_remote_port != remote_port) {
        continue;
      }
      file->udp_error = (uint32_t)error;
      cell_socket_wake_file(file);
      return true;
    }
  }
  return false;
}

bool cell_net_deliver_tcp_error(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, int error) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { continue; }
      if (file->tcp_local_port != local_port || file->tcp_remote_ip != remote_ip ||
          file->tcp_remote_port != remote_port) {
        continue;
      }
      file->tcp_error = (uint8_t)error;
      file->tcp_state = TCP_CLOSED;
      file->tcp_retransmit_deadline_tick = 0;
      file->tcp_syn_sent_tick = 0;
      tcp_tx_clear(file);
      cell_socket_wake_file(file);
      return true;
    }
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
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
          file->tcp_state == TCP_LISTEN && file->tcp_local_port == port) {
        return file;
      }
    }
  }
  return NULL;
}

static void tcp_deliver_syn_to_listener(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq,
                                        uint16_t window, const void *options, size_t options_len) {
  struct open_file *listener = tcp_listener_for_port(dst_port);
  if (listener == NULL) {
    (void)net_tcp_send_segment(dst_port, src_ip, src_port, 0, seq + 1, 0, 0x14, NULL, 0);
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
  server->tcp_nodelay = listener->tcp_nodelay;
  server->so_sndbuf = listener->so_sndbuf;
  server->so_rcvbuf = listener->so_rcvbuf;
  server->so_rcvtimeo_ticks = listener->so_rcvtimeo_ticks;
  server->so_sndtimeo_ticks = listener->so_sndtimeo_ticks;
  if (!tcp_alloc_rx(server)) {
    cell_release_open_file(server);
    return;
  }
  server->tcp_local_port = dst_port;
  server->tcp_remote_ip = src_ip;
  server->tcp_remote_port = src_port;
  server->tcp_seq = tcp_initial_seq(NULL, 0);
  server->tcp_ack = seq + 1;
  server->tcp_remote_window_scale = tcp_parse_window_scale(options, options_len);
  server->tcp_remote_mss = tcp_parse_mss(options, options_len);
  tcp_update_remote_window(server, window);
  tcp_init_rto(server);
  server->tcp_state = TCP_SYN_RECEIVED;
  uint32_t syn_seq = server->tcp_seq;
  server->tcp_seq += 1;
  tcp_pending[pending_slot] = (struct tcp_pending_conn){.used = true, .listener = listener, .server_end = server};
  if (!net_tcp_send_segment(server->tcp_local_port, server->tcp_remote_ip, server->tcp_remote_port, syn_seq,
                            server->tcp_ack, tcp_syn_window(server), 0x12, NULL, 0)) {
    cell_release_open_file(server);
    tcp_pending[pending_slot] = (struct tcp_pending_conn){0};
  }
}

static uint32_t tcp_segment_ack_delta(uint8_t flags, size_t len) {
  uint32_t delta = (uint32_t)len;
  if ((flags & 0x02) != 0) { ++delta; }
  if ((flags & 0x01) != 0) { ++delta; }
  return delta;
}

static void tcp_send_reset_for_unmatched(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq,
                                         uint32_t ack, uint8_t flags, size_t len) {
  if ((flags & 0x04) != 0) { return; }
  if ((flags & 0x10) != 0) {
    (void)net_tcp_send_segment(dst_port, src_ip, src_port, ack, 0, 0, 0x04, NULL, 0);
    return;
  }
  (void)net_tcp_send_segment(dst_port, src_ip, src_port, 0, seq + tcp_segment_ack_delta(flags, len), 0, 0x14, NULL, 0);
}

static bool tcp_deliver_to_file(struct open_file *file, uint32_t seq, uint32_t ack, uint16_t window, uint8_t flags,
                                const void *options, size_t options_len, const void *payload, size_t len) {
  if (file->tcp_state == TCP_SYN_SENT && (flags & 0x12) == 0x12 && ack == file->tcp_seq) {
    if (file->tcp_syn_retries == 0) { tcp_note_rtt_sample(file, file->tcp_syn_sent_tick, cell_uptime_ticks()); }
    file->tcp_remote_window_scale = tcp_parse_window_scale(options, options_len);
    file->tcp_remote_mss = tcp_parse_mss(options, options_len);
    tcp_update_remote_window(file, window);
    file->tcp_ack = seq + 1;
    file->tcp_state = TCP_ESTABLISHED;
    file->tcp_syn_retries = 0;
    file->tcp_retransmit_deadline_tick = 0;
    file->tcp_syn_sent_tick = 0;
    (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                               file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
    cell_socket_wake_file(file);
    return true;
  }
  if (file->tcp_state == TCP_SYN_RECEIVED && (flags & 0x10) != 0 && ack == file->tcp_seq) {
    tcp_update_remote_window(file, window);
    file->tcp_state = TCP_ESTABLISHED;
    cell_socket_wake_file(file);
    for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
      if (tcp_pending[i].used && tcp_pending[i].server_end == file) {
        cell_socket_wake_file(tcp_pending[i].listener);
        break;
      }
    }
    return true;
  }
  if ((flags & 0x04) != 0) {
    file->tcp_error = file->tcp_state == TCP_SYN_SENT ? ECONNREFUSED : ECONNRESET;
    file->tcp_state = TCP_CLOSED;
    file->tcp_retransmit_deadline_tick = 0;
    file->tcp_syn_sent_tick = 0;
    tcp_tx_clear(file);
    cell_socket_wake_file(file);
    return true;
  }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return true; }
  tcp_update_remote_window(file, window);
  if ((flags & 0x10) != 0) { tcp_tx_ack(file, ack); }
  uint32_t packet_fin_seq = seq + (uint32_t)len;
  if (len != 0) {
    const uint8_t *data = payload;
    uint32_t data_len = (uint32_t)len;
    if (seq_before(seq, file->tcp_ack)) {
      uint32_t skip = file->tcp_ack - seq;
      if (skip >= data_len) {
        (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                                   file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
        cell_socket_wake_file(file);
        return true;
      }
      data += skip;
      data_len -= skip;
      seq = file->tcp_ack;
    }
    if (seq != file->tcp_ack) {
      tcp_store_ooo(file, seq, data, data_len);
      if ((flags & 0x01) != 0) { tcp_record_fin(file, packet_fin_seq); }
      (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                                 file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
      cell_socket_wake_file(file);
      return true;
    }
    uint32_t n = tcp_append_rx(file, data, data_len);
    if (n != 0) {
      tcp_drain_ooo(file);
      if ((flags & 0x01) != 0) { tcp_record_fin(file, packet_fin_seq); }
      (void)tcp_accept_pending_fin(file);
      (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                                 file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
    } else {
      (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                                 file->tcp_ack, 0, 0x10, NULL, 0);
    }
  }
  if ((flags & 0x01) != 0) {
    tcp_record_fin(file, packet_fin_seq);
    if (tcp_accept_pending_fin(file)) {
      (void)net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port, file->tcp_seq,
                                 file->tcp_ack, tcp_window(file), 0x10, NULL, 0);
    }
  }
  cell_socket_wake_file(file);
  return true;
}

void cell_net_deliver_tcp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint16_t window, uint8_t flags, const void *options, size_t options_len,
                          const void *payload, size_t len) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (!socket_matches_tcp(file, src_ip, src_port, dst_port)) { continue; }
      (void)tcp_deliver_to_file(file, seq, ack, window, flags, options, options_len, payload, len);
      return;
    }
  }
  for (size_t i = 0; i < sizeof(tcp_pending) / sizeof(tcp_pending[0]); ++i) {
    if (!tcp_pending[i].used || !socket_matches_tcp(tcp_pending[i].server_end, src_ip, src_port, dst_port)) {
      continue;
    }
    (void)tcp_deliver_to_file(tcp_pending[i].server_end, seq, ack, window, flags, options, options_len, payload, len);
    return;
  }
  if ((flags & 0x02) != 0 && (flags & 0x10) == 0) {
    tcp_deliver_syn_to_listener(src_ip, src_port, dst_port, seq, window, options, options_len);
    return;
  }
  tcp_send_reset_for_unmatched(src_ip, src_port, dst_port, seq, ack, flags, len);
}

void cell_net_deliver_icmp(uint32_t src_ip, const void *payload, size_t len) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_ICMP) { continue; }
      if (dgram_push_rx(file, src_ip, 0, payload, len)) { cell_socket_wake_file(file); }
      return;
    }
  }
}

static void clear_socket_wait(struct thread *thread) {
  thread->wait_reason = WAIT_NONE;
  thread->wait_target = -1;
  thread->pipe_buf = 0;
  thread->pipe_len = 0;
  thread->socket_addr = 0;
  thread->socket_addrlen = 0;
  thread->socket_accept_flags = 0;
  thread->socket_write = false;
  thread->socket_msg = false;
  thread->socket_msg_addr = 0;
  thread->socket_iov = 0;
  thread->socket_iovlen = 0;
  thread->socket_flags = 0;
  thread->socket_has_deadline = false;
  thread->socket_deadline_tick = 0;
}

static void wake_socket_timeout_waiters(uint64_t now_ticks) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_SOCKET ||
        !thread->socket_has_deadline || now_ticks < thread->socket_deadline_tick) {
      continue;
    }
    thread->tf.x[0] = (uint64_t)(-(int64_t)EAGAIN);
    thread->state = THREAD_RUNNABLE;
    clear_socket_wait(thread);
  }
}

static void wake_socket_waiters(struct open_file *file) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_SOCKET ||
        thread->domain == NULL) {
      continue;
    }
    int fd = thread->wait_target;
    if (fd >= 0 && fd < MAX_FDS && thread->domain->fds[fd] == file) {
      if (thread->socket_write && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
        int64_t rc = thread->socket_msg ? socket_sendmsg_tcp_from_domain(thread->domain, file, thread->socket_msg_addr)
                                        : cell_socket_tcp_write_from_domain(thread->domain, file, thread->pipe_buf,
                                                                            thread->pipe_len);
        if (rc == -EAGAIN && tcp_send_would_block(file)) { continue; }
        thread->tf.x[0] = (uint64_t)rc;
      } else if (thread->socket_msg && file->type == OPEN_SOCKET) {
        int64_t rc = socket_recvmsg_to_iov(thread->domain, file, thread->socket_msg_addr, thread->socket_iov,
                                          (int32_t)thread->socket_iovlen, thread->socket_addr,
                                          thread->socket_addrlen, thread->socket_flags);
        if (rc == -EAGAIN) { continue; }
        thread->tf.x[0] = (uint64_t)rc;
      } else if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP) {
        if (thread->pipe_buf == 0 && thread->pipe_len == 0) {
          if (file->tcp_state == TCP_LISTEN) {
            int accepted = cell_socket_take_pending_tcp(thread->domain, file, thread->socket_accept_flags);
            if (accepted == -EAGAIN) { continue; }
            if (accepted < 0) {
              thread->tf.x[0] = (uint64_t)(int64_t)accepted;
              thread->state = THREAD_RUNNABLE;
              clear_socket_wait(thread);
              continue;
            }
            int addr_rc = tcp_copy_peer_addr_to_domain(thread->domain, accepted, thread->socket_addr,
                                                       thread->socket_addrlen);
            if (addr_rc < 0) {
              socket_close_domain_fd(thread->domain, accepted);
              thread->tf.x[0] = (uint64_t)(int64_t)addr_rc;
              thread->state = THREAD_RUNNABLE;
              clear_socket_wait(thread);
              continue;
            }
            socket_apply_accept_flags(thread->domain, accepted, thread->socket_accept_flags);
            thread->tf.x[0] = (uint64_t)(int64_t)accepted;
          } else if (file->tcp_error != 0) {
            thread->tf.x[0] = (uint64_t)(-(int64_t)file->tcp_error);
          } else if (file->tcp_state != TCP_ESTABLISHED) {
            continue;
          } else {
            thread->tf.x[0] = 0;
          }
        } else {
          if (tcp_recv_waitall_pending(file, thread->pipe_len, thread->socket_flags)) { continue; }
          int64_t rc =
            cell_socket_tcp_read_to_domain_flags(thread->domain, file, thread->pipe_buf, thread->pipe_len,
                                                 thread->socket_flags);
          if (rc == -EAGAIN) { continue; }
          thread->tf.x[0] = (uint64_t)rc;
        }
      } else if (file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_UDP &&
                 (file->dgram_rx_count != 0 || file->udp_error != 0) && thread->pipe_buf != 0) {
        int64_t rc = dgram_read_to_domain(thread->domain, file, thread->pipe_buf, thread->pipe_len, thread->socket_addr,
                                          thread->socket_addrlen, thread->socket_flags);
        if (rc == -EAGAIN) { continue; }
        thread->tf.x[0] = (uint64_t)rc;
      }
      thread->state = THREAD_RUNNABLE;
      clear_socket_wait(thread);
    }
  }
  cell_wake_poll_waiters_internal();
}

void cell_socket_wake_file(struct open_file *file) {
  wake_socket_waiters(file);
}

void cell_socket_timer_tick(uint64_t now_ticks) {
  wake_socket_timeout_waiters(now_ticks);
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    struct domain *domain = cell_domain_slot(d);
    if (domain == NULL || !domain->used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domain->fds[fd];
      if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { continue; }
      if (file->tcp_state == TCP_SYN_SENT) {
        if (file->tcp_retransmit_deadline_tick == 0 || now_ticks < file->tcp_retransmit_deadline_tick) { continue; }
        if (file->tcp_syn_retries >= TCP_SYN_MAX_RETRIES) {
          file->tcp_error = ETIMEDOUT;
          file->tcp_state = TCP_CLOSED;
          file->tcp_retransmit_deadline_tick = 0;
          file->tcp_syn_sent_tick = 0;
          tcp_tx_clear(file);
          wake_socket_waiters(file);
          continue;
        }
        uint32_t syn_seq = file->tcp_seq - 1;
        if (!tcp_send_syn(file, syn_seq)) {
          file->tcp_error = EIO;
          file->tcp_state = TCP_CLOSED;
          file->tcp_retransmit_deadline_tick = 0;
          file->tcp_syn_sent_tick = 0;
          tcp_tx_clear(file);
          wake_socket_waiters(file);
          continue;
        }
        ++file->tcp_syn_retries;
        file->tcp_syn_sent_tick = now_ticks;
        file->tcp_retransmit_deadline_tick = now_ticks + tcp_backoff_rto(file, file->tcp_syn_retries);
        continue;
      }
      if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { continue; }
      if (file->tcp_tx_count == 0) { continue; }
      uint8_t index = file->tcp_tx_head;
      if (now_ticks < file->tcp_tx_deadline_tick[index]) { continue; }
      if (file->tcp_tx_retries[index] >= TCP_DATA_MAX_RETRIES) {
        file->tcp_error = ETIMEDOUT;
        file->tcp_state = TCP_CLOSED;
        tcp_tx_clear(file);
        wake_socket_waiters(file);
        continue;
      }
      uint8_t slot = file->tcp_tx_slot[index];
      if (!net_tcp_send_segment(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                file->tcp_tx_seq[index], file->tcp_ack, tcp_window(file), 0x18,
                                tcp_tx_buffers[slot], file->tcp_tx_len[index])) {
        file->tcp_error = EIO;
        file->tcp_state = TCP_CLOSED;
        tcp_tx_clear(file);
        wake_socket_waiters(file);
        continue;
      }
      ++file->tcp_tx_retries[index];
      file->tcp_tx_sent_tick[index] = now_ticks;
      file->tcp_tx_deadline_tick[index] = now_ticks + tcp_backoff_rto(file, file->tcp_tx_retries[index]);
    }
  }
}
