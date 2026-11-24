#include "internal.h"
#include "proc/epoll.h"
#include "lib/mem.h"
#include "mm/page_ring.h"
#include "mm/pmm.h"
#include "mm/pool.h"

static_assert(CELL_TCP_RX_CAP % PAGE_SIZE == 0 && CELL_TCP_RX_CAP / PAGE_SIZE <= 64);

struct tcp_rx_state {
  struct page_ring ring;
  uint64_t pages[CELL_TCP_RX_CAP / PAGE_SIZE];
};

static struct object_pool receivers;

uint32_t tcp_rx_available(const struct open_file *file) {
  return file->tcp_rx ? file->tcp_rx->ring.used : 0;
}

void tcp_free_rx(struct open_file *file) {
  if (!file->tcp_rx) { return; }
  page_ring_clear(&file->tcp_rx->ring);
  pool_free(&receivers, file->tcp_rx);
  file->tcp_rx = NULL;
}

static int copy_payload(void *source, uint8_t *dst, size_t len) {
  const uint8_t **data = source;
  kmemcpy(dst, *data, len);
  *data += len;
  return 0;
}

uint32_t tcp_append_rx(struct open_file *file, const uint8_t *data, uint32_t len) {
  uint32_t used = tcp_rx_available(file), limit = socket_effective_buffer(file, true);
  if (!len || used >= limit) { return 0; }
  if (!file->tcp_rx) {
    if (!receivers.limit && !pool_init(&receivers, sizeof(struct tcp_rx_state), SIZE_MAX)) { return 0; }
    file->tcp_rx = pool_alloc(&receivers);
    if (!file->tcp_rx) { return 0; }
    file->tcp_rx->ring = (struct page_ring){.pages = file->tcp_rx->pages, .capacity = CELL_TCP_RX_CAP};
  }
  if (len > limit - used) { len = limit - used; }
  int n = page_ring_write(&file->tcp_rx->ring, len, copy_payload, &data);
  if (n <= 0) { return 0; }
  file->tcp_ack += (uint32_t)n;
  return (uint32_t)n;
}

int64_t tcp_read_stream(struct open_file *file, uint64_t len, uint32_t flags, socket_copy_out copy, void *destination) {
  if (!file || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (!len) { return 0; }
  uint32_t used = tcp_rx_available(file);
  if (!used) {
    if (file->tcp_error) { return -(int)file->tcp_error; }
    return file->tcp_fin ? 0 : -EAGAIN;
  }
  bool peek = (flags & MSG_PEEK) != 0;
  int rc = page_ring_read(&file->tcp_rx->ring, len < used ? (uint32_t)len : used, peek, copy, destination);
  if (rc > 0 && !peek) {
    tcp_drain_ooo(file);
    cell_epoll_file_changed(file);
    tcp_ack_current_state(file);
  }
  return rc;
}
