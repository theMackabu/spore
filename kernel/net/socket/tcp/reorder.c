#include "internal.h"
#include "lib/mem.h"
#include "mm/pool.h"

constexpr unsigned MAX_FRAGMENTS = 8;

struct tcp_fragment {
  struct tcp_fragment *next;
  uint32_t seq;
  uint16_t len;
  uint8_t data[CELL_TCP_TX_CAP];
};

static struct object_pool fragments;

static void drop_first(struct open_file *file) {
  struct tcp_fragment *fragment = file->tcp_ooo;
  file->tcp_ooo = fragment->next;
  --file->tcp_ooo_count;
  pool_free(&fragments, fragment);
}

void tcp_free_ooo(struct open_file *file) {
  while (file->tcp_ooo) { drop_first(file); }
}

static void store(struct open_file *file, uint32_t seq, const uint8_t *data, uint32_t len) {
  struct tcp_fragment **link = &file->tcp_ooo;
  while (*link && (int32_t)((*link)->seq - seq) < 0) { link = &(*link)->next; }
  struct tcp_fragment *fragment = *link;
  if (len > CELL_TCP_TX_CAP) { len = CELL_TCP_TX_CAP; }
  if (fragment && fragment->seq == seq) {
    if (len <= fragment->len) { return; }
  } else {
    if (file->tcp_ooo_count >= MAX_FRAGMENTS) { return; }
    if (!fragments.limit && !pool_init(&fragments, sizeof(*fragment), SIZE_MAX)) { return; }
    fragment = pool_alloc(&fragments);
    if (!fragment) { return; }
    fragment->seq = seq;
    fragment->next = *link;
    *link = fragment;
    ++file->tcp_ooo_count;
  }
  kmemcpy(fragment->data, data, len);
  fragment->len = len;
}

void tcp_drain_ooo(struct open_file *file) {
  while (file->tcp_ooo && !file->tcp_fin) {
    struct tcp_fragment *fragment = file->tcp_ooo;
    int32_t offset = (int32_t)(file->tcp_ack - fragment->seq);
    if (offset < 0) { break; }
    if ((uint32_t)offset < fragment->len) {
      uint32_t len = fragment->len - (uint32_t)offset;
      if (file->tcp_fin_pending && len > file->tcp_fin_seq - file->tcp_ack) { len = file->tcp_fin_seq - file->tcp_ack; }
      if (tcp_append_rx(file, fragment->data + offset, len) < len) { break; }
    }
    drop_first(file);
  }
  if (file->tcp_fin_pending && file->tcp_fin_seq == file->tcp_ack) {
    file->tcp_fin_pending = false;
    file->tcp_fin = true;
    ++file->tcp_ack;
    tcp_free_ooo(file);
  }
}

void tcp_receive_segment(struct open_file *file, uint32_t seq, const uint8_t *data, uint32_t len, bool fin) {
  if (file->tcp_fin) { return; }
  uint32_t end = seq + len;
  uint32_t limit = socket_effective_buffer(file, true);
  uint32_t used = tcp_rx_available(file);
  uint32_t room = used < limit ? limit - used : 0;
  if (fin && end - file->tcp_ack <= room) {
    file->tcp_fin_pending = true;
    file->tcp_fin_seq = end;
  }
  int32_t gap = (int32_t)(seq - file->tcp_ack);
  if (gap > 0) {
    if ((uint32_t)gap < room && len) {
      uint32_t available = room - (uint32_t)gap;
      store(file, seq, data, len < available ? len : available);
    }
    return;
  }
  uint32_t skip = file->tcp_ack - seq;
  if (len > skip) {
    len -= skip;
    if (file->tcp_fin_pending && len > file->tcp_fin_seq - file->tcp_ack) { len = file->tcp_fin_seq - file->tcp_ack; }
    tcp_append_rx(file, data + skip, len);
  }
  tcp_drain_ooo(file);
}
