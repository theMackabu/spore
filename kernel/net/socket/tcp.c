#include "internal.h"

#include "proc/accounting.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "net/net.h"
#include "net/rust.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "proc/thread.h"
#include "lib/random.h"

enum {
  EINPROGRESS = 115,
  EOPNOTSUPP = 95,
  ECONNRESET = 104,
  ETIMEDOUT = 110,
  TCP_OOO_BUFFER_SLOTS = 32,
  TCP_RX_BUFFER_SLOTS = 16,
  TCP_PORT_BUCKETS = 32,
  TCP_OOO_ENTRIES = 8,
  TCP_OOO_PAYLOAD_CAP = 1460,
  TCP_OOO_STATE_PAGES = (TCP_OOO_ENTRIES + 1) / 2,
  TCP_RCV_WINDOW_SCALE = 2,
  TCP_DEFAULT_REMOTE_WINDOW = 65535,
  TCP_DEFAULT_MSS = 1460,
  TCP_SEND_STACK_CHUNK = 1460,
  TCP_INITIAL_RTO_TICKS = 100,
  TCP_MIN_RTO_TICKS = 10,
  TCP_MAX_RTO_TICKS = 6000,
  TCP_MAX_RTO_SHIFT = 6,
  TCP_SYN_MAX_RETRIES = 5,
  TCP_DATA_MAX_RETRIES = 8,
  TCP_TICKS_PER_SECOND = 100,
  TCP_DEFAULT_KEEPIDLE_SEC = 7200,
  TCP_DEFAULT_KEEPINTVL_SEC = 75,
  TCP_DEFAULT_KEEPCNT = 9,
  TCP_FIN = 0x01,
  TCP_SYN = 0x02,
  TCP_RST = 0x04,
  TCP_PSH = 0x08,
  TCP_ACK = 0x10,
};

struct tcp_tx_state {
  uint8_t head;
  uint8_t count;
  uint16_t len[CELL_TCP_TX_QUEUE_CAP];
  uint32_t seq[CELL_TCP_TX_QUEUE_CAP];
  uint8_t retries[CELL_TCP_TX_QUEUE_CAP];
  uint64_t deadline_tick[CELL_TCP_TX_QUEUE_CAP];
  uint64_t sent_tick[CELL_TCP_TX_QUEUE_CAP];
  uint64_t first_sent_tick[CELL_TCP_TX_QUEUE_CAP];
  uint8_t payload[CELL_TCP_TX_QUEUE_CAP][CELL_TCP_TX_CAP];
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

struct tcp_rx_state {
  struct open_file *owner;
  uint32_t head;
  bool timer_active;
  uint64_t pages[CELL_TCP_RX_CAP / PAGE_SIZE];
};

static struct tcp_pending_conn tcp_pending[16];

static struct open_file *tcp_port_buckets[TCP_PORT_BUCKETS];

static struct tcp_rx_state tcp_rx_buffers[TCP_RX_BUFFER_SLOTS];

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

static struct tcp_rx_state *tcp_rx_state_for(const struct open_file *file) {
  if (file == NULL || file->tcp_rx_slot == 0) { return NULL; }
  size_t slot = (size_t)file->tcp_rx_slot - 1;
  return slot < TCP_RX_BUFFER_SLOTS && tcp_rx_buffers[slot].owner == file ? &tcp_rx_buffers[slot] : NULL;
}

static bool tcp_alloc_rx(struct open_file *file) {
  if (file == NULL) { return false; }
  if (file->tcp_rx_slot != 0) { return tcp_rx_state_for(file) != NULL; }
  for (size_t i = 0; i < TCP_RX_BUFFER_SLOTS; ++i) {
    if (tcp_rx_buffers[i].owner != NULL) { continue; }
    tcp_rx_buffers[i].timer_active = true;
    tcp_rx_buffers[i].owner = file;
    file->tcp_rx_slot = (uint8_t)(i + 1);
    return true;
  }
  return false;
}

static void tcp_reset_rx(struct open_file *file) {
  struct tcp_rx_state *state = tcp_rx_state_for(file);
  if (state == NULL) { return; }
  for (size_t i = 0; i < sizeof(state->pages) / sizeof(state->pages[0]); ++i) {
    if (state->pages[i] != 0) { pmm_free_page(state->pages[i]); }
  }
  kmemset(state->pages, 0, sizeof(state->pages));
  state->head = 0;
  file->tcp_rx_len = 0;
}

static void tcp_free_rx(struct open_file *file) {
  struct tcp_rx_state *state = tcp_rx_state_for(file);
  if (state == NULL) { return; }
  tcp_reset_rx(file);
  state->timer_active = false;
  state->owner = NULL;
  file->tcp_rx_slot = 0;
}

static void tcp_set_timer_active(struct open_file *file, bool active) {
  struct tcp_rx_state *state = tcp_rx_state_for(file);
  if (state != NULL) { state->timer_active = active; }
}

static uint8_t *tcp_rx_page(struct open_file *file, uint32_t offset, bool create) {
  struct tcp_rx_state *state = tcp_rx_state_for(file);
  if (state == NULL) { return NULL; }
  size_t index = offset / PAGE_SIZE;
  if (index >= sizeof(state->pages) / sizeof(state->pages[0])) { return NULL; }
  uint64_t pa = state->pages[index];
  if (pa == 0 && create) {
    pa = pmm_alloc_zero_page();
    if (pa == 0) { return NULL; }
    state->pages[index] = pa;
  }
  return pa == 0 ? NULL : pmm_phys_to_virt(pa);
}

static bool tcp_rx_write(struct open_file *file, uint32_t offset, const uint8_t *src, uint32_t len) {
  while (len != 0) {
    uint32_t page_offset = offset % PAGE_SIZE;
    uint32_t chunk = PAGE_SIZE - page_offset;
    if (chunk > len) { chunk = len; }
    uint8_t *page = tcp_rx_page(file, offset, true);
    if (page == NULL) { return false; }
    kmemcpy(page + page_offset, src, chunk);
    src += chunk;
    len -= chunk;
    offset = (offset + chunk) % CELL_TCP_RX_CAP;
  }
  return true;
}

static bool tcp_rx_copy_to_user(struct domain *domain, const struct open_file *file, uint64_t dst, uint32_t len) {
  struct tcp_rx_state *state = tcp_rx_state_for(file);
  if (state == NULL) { return false; }
  uint32_t offset = state->head;
  while (len != 0) {
    uint32_t page_offset = offset % PAGE_SIZE;
    uint32_t chunk = PAGE_SIZE - page_offset;
    if (chunk > len) { chunk = len; }
    uint8_t *page = tcp_rx_page((struct open_file *)file, offset, false);
    if (page == NULL || !vmm_copy_to_user(cell_domain_as(domain), dst, page + page_offset, chunk)) { return false; }
    dst += chunk;
    len -= chunk;
    offset = (offset + chunk) % CELL_TCP_RX_CAP;
  }
  return true;
}

static bool tcp_rx_copy_to_kernel(const struct open_file *file, void *dst, uint32_t len) {
  uint8_t *out = dst;
  struct tcp_rx_state *state = tcp_rx_state_for(file);
  if (state == NULL) { return false; }
  uint32_t offset = state->head;
  while (len != 0) {
    uint32_t page_offset = offset % PAGE_SIZE;
    uint32_t chunk = PAGE_SIZE - page_offset;
    if (chunk > len) { chunk = len; }
    uint8_t *page = tcp_rx_page((struct open_file *)file, offset, false);
    if (page == NULL) { return false; }
    kmemcpy(out, page + page_offset, chunk);
    out += chunk;
    len -= chunk;
    offset = (offset + chunk) % CELL_TCP_RX_CAP;
  }
  return true;
}

static void tcp_rx_consume(struct open_file *file, uint32_t len) {
  struct tcp_rx_state *state = tcp_rx_state_for(file);
  if (state == NULL) { return; }
  state->head = (state->head + len) % CELL_TCP_RX_CAP;
  file->tcp_rx_len -= len;
  if (file->tcp_rx_len == 0) { tcp_reset_rx(file); }
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

static uint64_t tcp_tx_state_pages(void) {
  return ((uint64_t)sizeof(struct tcp_tx_state) + PAGE_SIZE - 1) / PAGE_SIZE;
}

static bool tcp_alloc_tx(struct open_file *file) {
  if (file == NULL) { return false; }
  if (file->tcp_tx != NULL) { return true; }
  uint64_t pages = tcp_tx_state_pages();
  uint64_t pa = pmm_alloc_contiguous_pages(pages);
  if (pa == 0) { return false; }
  void *ptr = pmm_phys_to_virt(pa);
  if (ptr == NULL) {
    for (uint64_t i = 0; i < pages; ++i) {
      pmm_free_page(pa + i * PAGE_SIZE);
    }
    return false;
  }
  kmemset(ptr, 0, (size_t)(pages * PAGE_SIZE));
  file->tcp_tx_pa = pa;
  file->tcp_tx = ptr;
  return true;
}

static void tcp_free_tx(struct open_file *file) {
  if (file == NULL || file->tcp_tx == NULL || file->tcp_tx_pa == 0) { return; }
  uint64_t pages = tcp_tx_state_pages();
  kmemset(file->tcp_tx, 0, (size_t)(pages * PAGE_SIZE));
  for (uint64_t i = 0; i < pages; ++i) {
    pmm_free_page(file->tcp_tx_pa + i * PAGE_SIZE);
  }
  file->tcp_tx_pa = 0;
  file->tcp_tx = NULL;
}

static uint8_t tcp_tx_count(const struct open_file *file) {
  return file != NULL && file->tcp_tx != NULL ? file->tcp_tx->count : 0;
}

static uint8_t tcp_tx_queue_index(const struct open_file *file, uint8_t offset) {
  return (uint8_t)((file->tcp_tx->head + offset) % CELL_TCP_TX_QUEUE_CAP);
}

static void tcp_tx_drop_head(struct open_file *file) {
  if (file == NULL || file->tcp_tx == NULL || file->tcp_tx->count == 0) { return; }
  struct tcp_tx_state *tx = file->tcp_tx;
  uint8_t index = tx->head;
  tx->len[index] = 0;
  tx->seq[index] = 0;
  tx->retries[index] = 0;
  tx->deadline_tick[index] = 0;
  tx->sent_tick[index] = 0;
  tx->first_sent_tick[index] = 0;
  tx->head = tcp_tx_queue_index(file, 1);
  --tx->count;
  if (tx->count == 0) { tx->head = 0; }
}

static void tcp_tx_drop_tail(struct open_file *file) {
  if (file == NULL || file->tcp_tx == NULL || file->tcp_tx->count == 0) { return; }
  struct tcp_tx_state *tx = file->tcp_tx;
  uint8_t index = tcp_tx_queue_index(file, (uint8_t)(tx->count - 1));
  tx->len[index] = 0;
  tx->seq[index] = 0;
  tx->retries[index] = 0;
  tx->deadline_tick[index] = 0;
  tx->sent_tick[index] = 0;
  tx->first_sent_tick[index] = 0;
  --tx->count;
  if (tx->count == 0) { tx->head = 0; }
}

static void tcp_tx_clear(struct open_file *file) {
  if (file == NULL) { return; }
  while (tcp_tx_count(file) != 0) {
    tcp_tx_drop_head(file);
  }
}

static uint16_t tcp_window(const struct open_file *file) {
  uint32_t limit = socket_effective_buffer(file, true);
  uint32_t room = file->tcp_rx_len >= limit ? 0 : limit - file->tcp_rx_len;
  uint32_t scaled = room >> TCP_RCV_WINDOW_SCALE;
  return scaled > UINT16_MAX ? UINT16_MAX : (uint16_t)scaled;
}

static uint16_t tcp_syn_window(const struct open_file *file) {
  uint32_t limit = socket_effective_buffer(file, true);
  uint32_t room = file->tcp_rx_len >= limit ? 0 : limit - file->tcp_rx_len;
  return room > UINT16_MAX ? UINT16_MAX : (uint16_t)room;
}

static bool tcp_send_syn(struct open_file *file, uint32_t seq) {
  uint8_t options[8] = {
    2, 4, 0x05, 0xb4, 1, 3, 3, TCP_RCV_WINDOW_SCALE,
  };
  return net_tcp_send_segment_options_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                              socket_effective_ttl(file), (uint8_t)file->ip_tos, seq, 0,
                                              tcp_syn_window(file), TCP_SYN, NULL, 0, options, sizeof(options));
}

static int tcp_send_fin_once(struct open_file *file) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_error != 0) { return -(int)file->tcp_error; }
  if (file->tcp_fin_sent) { return 0; }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return -ENOTCONN; }
  if (!net_tcp_send_segment_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                    socket_effective_ttl(file), (uint8_t)file->ip_tos, file->tcp_seq, file->tcp_ack,
                                    tcp_window(file), TCP_FIN | TCP_ACK, NULL, 0)) {
    return -EIO;
  }
  file->tcp_fin_sent = true;
  file->tcp_state = TCP_FIN_WAIT;
  ++file->tcp_seq;
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

static uint32_t tcp_append_rx(struct open_file *file, const uint8_t *data, uint32_t len) {
  uint32_t limit = socket_effective_buffer(file, true);
  uint32_t room = file->tcp_rx_len >= limit ? 0 : limit - file->tcp_rx_len;
  uint32_t n = len < room ? len : room;
  if (n == 0) { return 0; }
  struct tcp_rx_state *state = tcp_rx_state_for(file);
  if (state == NULL) { return 0; }
  uint32_t tail = (state->head + file->tcp_rx_len) % CELL_TCP_RX_CAP;
  if (!tcp_rx_write(file, tail, data, n)) { return 0; }
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

static uint64_t tcp_user_timeout_ticks(const struct open_file *file) {
  if (file == NULL || file->tcp_user_timeout_ms == 0) { return 0; }
  return ((uint64_t)file->tcp_user_timeout_ms + 9u) / 10u;
}

static bool tcp_user_timeout_expired(const struct open_file *file, uint64_t now_ticks) {
  if (file == NULL || tcp_tx_count(file) == 0) { return false; }
  uint64_t timeout = tcp_user_timeout_ticks(file);
  if (timeout == 0) { return false; }
  const struct tcp_tx_state *tx = file->tcp_tx;
  uint64_t first_sent = tx->first_sent_tick[tx->head];
  return first_sent != 0 && now_ticks >= first_sent + timeout;
}

static uint64_t tcp_keepalive_ticks(uint32_t seconds) {
  return seconds == 0 ? 0 : (uint64_t)seconds * TCP_TICKS_PER_SECOND;
}

static void tcp_note_activity(struct open_file *file, uint64_t now_ticks) {
  if (file == NULL) { return; }
  file->tcp_last_activity_tick = now_ticks;
  file->tcp_keepalive_deadline_tick = 0;
  file->tcp_keepalive_probes = 0;
}

static void tcp_init_keepalive(struct open_file *file, uint64_t now_ticks) {
  if (file == NULL) { return; }
  file->tcp_keepidle = TCP_DEFAULT_KEEPIDLE_SEC;
  file->tcp_keepintvl = TCP_DEFAULT_KEEPINTVL_SEC;
  file->tcp_keepcnt = TCP_DEFAULT_KEEPCNT;
  tcp_note_activity(file, now_ticks);
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
  if (file == NULL || tcp_tx_count(file) == 0) { return 0; }
  const struct tcp_tx_state *tx = file->tcp_tx;
  return file->tcp_seq - tx->seq[tx->head];
}

static uint32_t tcp_send_window_available(const struct open_file *file) {
  if (file == NULL) { return 0; }
  uint32_t in_flight = tcp_tx_inflight(file);
  return file->tcp_remote_window > in_flight ? file->tcp_remote_window - in_flight : 0;
}

static uint32_t tcp_tx_queued_bytes(const struct open_file *file) {
  uint32_t total = 0;
  if (file == NULL || file->tcp_tx == NULL) { return 0; }
  const struct tcp_tx_state *tx = file->tcp_tx;
  for (uint8_t i = 0; i < tx->count; ++i) {
    total += tx->len[tcp_tx_queue_index(file, i)];
  }
  return total;
}

static uint32_t tcp_send_buffer_available(const struct open_file *file) {
  uint32_t limit = socket_effective_buffer(file, false);
  uint32_t queued = tcp_tx_queued_bytes(file);
  return queued >= limit ? 0 : limit - queued;
}

uint32_t tcp_send_chunk_limit(const struct open_file *file) {
  uint32_t mss = file == NULL || file->tcp_remote_mss == 0 ? TCP_DEFAULT_MSS : file->tcp_remote_mss;
  uint32_t limit = mss < TCP_SEND_STACK_CHUNK ? mss : TCP_SEND_STACK_CHUNK;
  uint32_t sendbuf = tcp_send_buffer_available(file);
  return sendbuf < limit ? sendbuf : limit;
}

static bool tcp_tx_track_segment(struct open_file *file, uint32_t seq, const uint8_t *data, uint16_t len) {
  if (file == NULL || len == 0 || tcp_tx_count(file) >= CELL_TCP_TX_QUEUE_CAP) { return false; }
  if (!tcp_alloc_tx(file)) { return false; }
  struct tcp_tx_state *tx = file->tcp_tx;
  uint8_t index = tcp_tx_queue_index(file, tx->count);
  tx->len[index] = len;
  tx->seq[index] = seq;
  tx->retries[index] = 0;
  uint64_t now_ticks = cell_uptime_ticks();
  tx->first_sent_tick[index] = now_ticks;
  tx->sent_tick[index] = now_ticks;
  tx->deadline_tick[index] = now_ticks + tcp_current_rto(file);
  kmemcpy(tx->payload[index], data, len);
  ++tx->count;
  return true;
}

static void tcp_tx_ack(struct open_file *file, uint32_t ack) {
  uint64_t now_ticks = cell_uptime_ticks();
  bool ambiguous = false;
  while (file != NULL && tcp_tx_count(file) != 0) {
    struct tcp_tx_state *tx = file->tcp_tx;
    uint8_t index = tx->head;
    uint32_t end = tx->seq[index] + tx->len[index];
    if (!seq_before_or_equal(end, ack)) { return; }
    if (tx->retries[index] == 0 && !ambiguous) {
      tcp_note_rtt_sample(file, tx->sent_tick[index], now_ticks);
    } else {
      ambiguous = true;
    }
    tcp_tx_drop_head(file);
  }
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

static void tcp_ack_current_state(struct open_file *file) {
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

bool tcp_send_would_block(const struct open_file *file) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
         file->tcp_state == TCP_ESTABLISHED && file->tcp_error == 0 && !file->tcp_fin_sent &&
         (tcp_tx_count(file) >= CELL_TCP_TX_QUEUE_CAP || tcp_send_window_available(file) == 0 ||
          tcp_send_buffer_available(file) == 0);
}

bool cell_tcp_socket_writable(const struct open_file *file) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP &&
         file->tcp_state == TCP_ESTABLISHED && file->tcp_error == 0 && !file->tcp_fin_sent &&
         tcp_tx_count(file) < CELL_TCP_TX_QUEUE_CAP && tcp_send_window_available(file) != 0 &&
         tcp_send_buffer_available(file) != 0;
}

int64_t tcp_write_one_from_kernel(struct open_file *file, const void *buf, uint64_t len) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_error != 0) { return -(int64_t)file->tcp_error; }
  if (file->tcp_fin_sent) { return -EPIPE; }
  if (file->tcp_state != TCP_ESTABLISHED) { return -ENOTCONN; }
  if (len == 0) { return 0; }
  uint32_t available = tcp_send_window_available(file);
  uint32_t sendbuf_available = tcp_send_buffer_available(file);
  if (sendbuf_available < available) { available = sendbuf_available; }
  if (available == 0) { return -EAGAIN; }
  uint64_t limit = tcp_send_chunk_limit(file);
  uint64_t chunk = len > limit ? limit : len;
  if (chunk > available) { chunk = available; }
  if (!tcp_tx_track_segment(file, file->tcp_seq, buf, (uint16_t)chunk)) { return -EAGAIN; }
  if (!net_tcp_send_segment_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                    socket_effective_ttl(file), (uint8_t)file->ip_tos, file->tcp_seq, file->tcp_ack,
                                    tcp_window(file), TCP_PSH | TCP_ACK, buf, (size_t)chunk)) {
    tcp_tx_drop_tail(file);
    return -EIO;
  }
  file->tcp_seq += (uint32_t)chunk;
  return (int64_t)chunk;
}

int64_t cell_socket_tcp_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len) {
  if (domain == NULL || file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_error != 0) { return -(int64_t)file->tcp_error; }
  if (file->tcp_fin_sent) { return -EPIPE; }
  if (file->tcp_state != TCP_ESTABLISHED) { return -ENOTCONN; }
  if (len == 0) { return 0; }

  uint8_t tmp[TCP_SEND_STACK_CHUNK];
  uint64_t sent = 0;
  while (sent < len) {
    uint64_t limit = tcp_send_chunk_limit(file);
    if (limit == 0) { return sent != 0 ? (int64_t)sent : -EAGAIN; }
    uint64_t chunk = len - sent;
    if (chunk > limit) { chunk = limit; }
    if (!vmm_copy_from_user(cell_domain_as(domain), tmp, buf + sent, (size_t)chunk)) {
      return sent != 0 ? (int64_t)sent : -EFAULT;
    }
    int64_t n = tcp_write_one_from_kernel(file, tmp, chunk);
    if (n < 0) { return sent != 0 ? (int64_t)sent : n; }
    if (n == 0) { break; }
    sent += (uint64_t)n;
    if ((uint64_t)n < chunk || !cell_tcp_socket_writable(file)) { break; }
  }
  return (int64_t)sent;
}

int64_t cell_socket_tcp_read_to_domain_flags(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len,
                                             uint32_t flags) {
  if (file->tcp_rx_len == 0) {
    if (file->tcp_error != 0) { return -(int64_t)file->tcp_error; }
    return file->tcp_fin ? 0 : -EAGAIN;
  }
  uint64_t n = file->tcp_rx_len < len ? file->tcp_rx_len : len;
  if (!tcp_rx_copy_to_user(domain, file, buf, (uint32_t)n)) { return -EFAULT; }
  if ((flags & MSG_PEEK) != 0) { return (int64_t)n; }
  tcp_rx_consume(file, (uint32_t)n);
  tcp_ack_current_state(file);
  return (int64_t)n;
}

int64_t cell_socket_tcp_read_to_kernel(struct open_file *file, void *buf, uint64_t len, uint32_t flags) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (file->tcp_rx_len == 0) {
    if (file->tcp_error != 0) { return -(int64_t)file->tcp_error; }
    return file->tcp_fin ? 0 : -EAGAIN;
  }
  uint64_t n = file->tcp_rx_len < len ? file->tcp_rx_len : len;
  if (n != 0 && !tcp_rx_copy_to_kernel(file, buf, (uint32_t)n)) { return -EIO; }
  if ((flags & MSG_PEEK) != 0) { return (int64_t)n; }
  tcp_rx_consume(file, (uint32_t)n);
  tcp_ack_current_state(file);
  return (int64_t)n;
}

bool tcp_recv_waitall_pending(const struct open_file *file, uint64_t want, uint32_t flags) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_TCP && (flags & MSG_WAITALL) != 0 &&
         want != 0 && file->tcp_rx_len < want && file->tcp_error == 0 && !file->tcp_fin;
}

bool tcp_init_socket(struct open_file *file) {
  file->tcp_remote_window = TCP_DEFAULT_REMOTE_WINDOW;
  file->tcp_remote_window_scale = 0;
  file->tcp_remote_mss = TCP_DEFAULT_MSS;
  tcp_init_rto(file);
  tcp_init_keepalive(file, cell_uptime_ticks());
  return tcp_alloc_rx(file);
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
  tcp_free_tx(file);
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
    tcp_note_activity(file, cell_uptime_ticks());
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
    tcp_reset_rx(file);
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
    file->tcp_error = (uint8_t)error;
    file->tcp_state = TCP_CLOSED;
    file->tcp_retransmit_deadline_tick = 0;
    file->tcp_syn_sent_tick = 0;
    tcp_tx_clear(file);
    cell_socket_wake_file(file);
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
  if (!tcp_alloc_rx(server)) {
    cell_release_open_file(server);
    return;
  }
  tcp_set_timer_active(server, false);
  server->tcp_local_port = dst_port;
  server->tcp_remote_ip = src_ip;
  server->tcp_remote_port = src_port;
  server->tcp_seq = tcp_initial_seq(NULL, 0);
  server->tcp_ack = seq + 1;
  struct net_tcp_options decoded = spore_net_decode_tcp_options(options, options_len);
  server->tcp_remote_window_scale = decoded.window_scale;
  server->tcp_remote_mss = decoded.mss;
  tcp_update_remote_window(server, window);
  tcp_init_rto(server);
  tcp_note_activity(server, cell_uptime_ticks());
  server->tcp_state = TCP_SYN_RECEIVED;
  uint32_t syn_seq = server->tcp_seq;
  server->tcp_seq += 1;
  tcp_pending[pending_slot] = (struct tcp_pending_conn){.used = true, .listener = listener, .server_end = server};
  if (!net_tcp_send_segment_ttl_tos(server->tcp_local_port, server->tcp_remote_ip, server->tcp_remote_port,
                                    socket_effective_ttl(server), (uint8_t)server->ip_tos, syn_seq, server->tcp_ack,
                                    tcp_syn_window(server), TCP_SYN | TCP_ACK, NULL, 0)) {
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
    struct net_tcp_options decoded = spore_net_decode_tcp_options(options, options_len);
    file->tcp_remote_window_scale = decoded.window_scale;
    file->tcp_remote_mss = decoded.mss;
    tcp_update_remote_window(file, window);
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
    tcp_update_remote_window(file, window);
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
    file->tcp_error = file->tcp_state == TCP_SYN_SENT ? ECONNREFUSED : ECONNRESET;
    file->tcp_state = TCP_CLOSED;
    file->tcp_retransmit_deadline_tick = 0;
    file->tcp_syn_sent_tick = 0;
    tcp_tx_clear(file);
    cell_socket_wake_file(file);
    return true;
  }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return true; }
  if ((flags & TCP_ACK) != 0 && tcp_ack_after_sent(file, ack)) {
    tcp_ack_current_state(file);
    return true;
  }
  tcp_note_activity(file, cell_uptime_ticks());
  tcp_update_remote_window(file, window);
  if ((flags & TCP_ACK) != 0) { tcp_tx_ack(file, ack); }
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

static void tcp_close_timed_out(struct open_file *file) {
  if (file == NULL) { return; }
  file->tcp_error = ETIMEDOUT;
  file->tcp_state = TCP_CLOSED;
  file->tcp_retransmit_deadline_tick = 0;
  file->tcp_keepalive_deadline_tick = 0;
  file->tcp_keepalive_probes = 0;
  tcp_tx_clear(file);
  wake_socket_waiters(file);
}

static void tcp_timer_keepalive(struct open_file *file, uint64_t now_ticks) {
  if (file == NULL || !file->so_keepalive || file->tcp_state != TCP_ESTABLISHED || tcp_tx_count(file) != 0) { return; }
  uint64_t idle_ticks = tcp_keepalive_ticks(file->tcp_keepidle);
  uint64_t interval_ticks = tcp_keepalive_ticks(file->tcp_keepintvl);
  if (idle_ticks == 0 || interval_ticks == 0 || file->tcp_keepcnt == 0) { return; }

  uint64_t due =
    file->tcp_keepalive_probes == 0 ? file->tcp_last_activity_tick + idle_ticks : file->tcp_keepalive_deadline_tick;
  if (now_ticks < due) { return; }
  if (file->tcp_keepalive_probes >= file->tcp_keepcnt) {
    tcp_close_timed_out(file);
    return;
  }

  uint32_t probe_seq = file->tcp_seq == 0 ? 0 : file->tcp_seq - 1u;
  if (!net_tcp_send_segment_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                    socket_effective_ttl(file), (uint8_t)file->ip_tos, probe_seq, file->tcp_ack,
                                    tcp_window(file), TCP_ACK, NULL, 0)) {
    file->tcp_error = EIO;
    file->tcp_state = TCP_CLOSED;
    wake_socket_waiters(file);
    return;
  }
  ++file->tcp_keepalive_probes;
  file->tcp_keepalive_deadline_tick = now_ticks + interval_ticks;
}

static void tcp_timer_file(struct open_file *file, uint64_t now_ticks) {
  if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return; }
  if (file->tcp_state == TCP_SYN_SENT) {
    if (file->tcp_retransmit_deadline_tick == 0 || now_ticks < file->tcp_retransmit_deadline_tick) { return; }
    if (file->tcp_syn_retries >= TCP_SYN_MAX_RETRIES) {
      file->tcp_error = ETIMEDOUT;
      file->tcp_state = TCP_CLOSED;
      file->tcp_retransmit_deadline_tick = 0;
      file->tcp_syn_sent_tick = 0;
      tcp_tx_clear(file);
      wake_socket_waiters(file);
      return;
    }
    uint32_t syn_seq = file->tcp_seq - 1;
    if (!tcp_send_syn(file, syn_seq)) {
      file->tcp_error = EIO;
      file->tcp_state = TCP_CLOSED;
      file->tcp_retransmit_deadline_tick = 0;
      file->tcp_syn_sent_tick = 0;
      tcp_tx_clear(file);
      wake_socket_waiters(file);
      return;
    }
    ++file->tcp_syn_retries;
    file->tcp_syn_sent_tick = now_ticks;
    file->tcp_retransmit_deadline_tick = now_ticks + tcp_backoff_rto(file, file->tcp_syn_retries);
    return;
  }
  if (file->tcp_state != TCP_ESTABLISHED && file->tcp_state != TCP_FIN_WAIT) { return; }
  if (tcp_tx_count(file) == 0) {
    tcp_timer_keepalive(file, now_ticks);
    return;
  }
  struct tcp_tx_state *tx = file->tcp_tx;
  uint8_t index = tx->head;
  if (tcp_user_timeout_expired(file, now_ticks)) {
    tcp_close_timed_out(file);
    return;
  }
  if (now_ticks < tx->deadline_tick[index]) { return; }
  if (tx->retries[index] >= TCP_DATA_MAX_RETRIES) {
    tcp_close_timed_out(file);
    return;
  }
  if (!net_tcp_send_segment_ttl_tos(file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                    socket_effective_ttl(file), (uint8_t)file->ip_tos, tx->seq[index], file->tcp_ack,
                                    tcp_window(file), TCP_PSH | TCP_ACK, tx->payload[index], tx->len[index])) {
    file->tcp_error = EIO;
    file->tcp_state = TCP_CLOSED;
    tcp_tx_clear(file);
    wake_socket_waiters(file);
    return;
  }
  ++tx->retries[index];
  tx->sent_tick[index] = now_ticks;
  tx->deadline_tick[index] = now_ticks + tcp_backoff_rto(file, tx->retries[index]);
}

void tcp_timer_tick(uint64_t now_ticks) {
  for (size_t i = 0; i < TCP_RX_BUFFER_SLOTS; ++i) {
    struct tcp_rx_state *state = &tcp_rx_buffers[i];
    struct open_file *file = state->owner;
    if (!state->timer_active || file == NULL || file->tcp_rx_slot != i + 1) { continue; }
    tcp_timer_file(file, now_ticks);
  }
}
