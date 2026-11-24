#include "internal.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "mm/pool.h"

enum { RX_PAGES = CELL_TCP_RX_CAP / PAGE_SIZE };

struct tcp_rx_state {
  uint32_t head;
  unsigned allocated;
  uint64_t pages[RX_PAGES];
};

static struct object_pool receivers;

void tcp_free_rx(struct open_file *file) {
  struct tcp_rx_state *rx = file->tcp_rx;
  if (!rx) { return; }
  for (size_t i = 0; i < RX_PAGES; ++i) {
    if (rx->pages[i]) { pmm_free_page(rx->pages[i]); }
  }
  pool_free(&receivers, rx);
  file->tcp_rx = NULL;
  file->tcp_rx_len = 0;
}

uint32_t tcp_append_rx(struct open_file *file, const uint8_t *data, uint32_t len) {
  uint32_t limit = socket_effective_buffer(file, true);
  if (!len || file->tcp_rx_len >= limit) { return 0; }
  if (len > limit - file->tcp_rx_len) { len = limit - file->tcp_rx_len; }
  if (!file->tcp_rx) {
    if (!receivers.limit && !pool_init(&receivers, sizeof(struct tcp_rx_state), SIZE_MAX)) { return 0; }
    file->tcp_rx = pool_alloc(&receivers);
    if (!file->tcp_rx) { return 0; }
  }
  struct tcp_rx_state *rx = file->tcp_rx;
  uint32_t offset = (rx->head + file->tcp_rx_len) % CELL_TCP_RX_CAP;
  uint32_t done = 0;
  while (done < len) {
    unsigned index = offset / PAGE_SIZE;
    uint32_t within = offset % PAGE_SIZE;
    uint32_t chunk = PAGE_SIZE - within;
    if (chunk > len - done) { chunk = len - done; }
    if (!rx->pages[index]) {
      rx->pages[index] = pmm_alloc_page();
      if (!rx->pages[index]) { break; }
      ++rx->allocated;
    }
    kmemcpy((uint8_t *)pmm_phys_to_virt(rx->pages[index]) + within, data + done, chunk);
    offset = (offset + chunk) % CELL_TCP_RX_CAP;
    done += chunk;
  }
  file->tcp_rx_len += done;
  file->tcp_ack += done;
  return done;
}

static void consume(struct open_file *file, uint32_t bytes) {
  struct tcp_rx_state *rx = file->tcp_rx;
  uint32_t start = rx->head;
  uint32_t end = start + bytes;
  rx->head = end % CELL_TCP_RX_CAP;
  file->tcp_rx_len -= bytes;
  if (!file->tcp_rx_len) {
    unsigned spare = ((end - 1) % CELL_TCP_RX_CAP) / PAGE_SIZE;
    uint64_t pa = rx->pages[spare];
    rx->pages[spare] = 0;
    if (rx->allocated > 1) {
      for (size_t i = 0; i < RX_PAGES; ++i) {
        if (rx->pages[i]) { pmm_free_page(rx->pages[i]); rx->pages[i] = 0; }
      }
    }
    rx->pages[0] = pa;
    rx->allocated = 1;
    rx->head = 0;
    return;
  }
  unsigned tail_page = ((end + file->tcp_rx_len - 1) % CELL_TCP_RX_CAP) / PAGE_SIZE;
  for (unsigned page = start / PAGE_SIZE; page < end / PAGE_SIZE; ++page) {
    unsigned index = page % RX_PAGES;
    if (index != tail_page) {
      pmm_free_page(rx->pages[index]);
      rx->pages[index] = 0;
      --rx->allocated;
    }
  }
}

int64_t tcp_read_stream(struct open_file *file, uint64_t len, uint32_t flags, socket_copy_out copy, void *destination) {
  if (!file || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_TCP) { return -9; }
  if (!len) { return 0; }
  if (!file->tcp_rx_len) {
    if (file->tcp_error) { return -(int)file->tcp_error; }
    return file->tcp_fin ? 0 : -EAGAIN;
  }
  uint32_t bytes = len < file->tcp_rx_len ? (uint32_t)len : file->tcp_rx_len;
  struct tcp_rx_state *rx = file->tcp_rx;
  uint32_t offset = rx->head;
  uint32_t done = 0;
  while (done < bytes) {
    uint32_t within = offset % PAGE_SIZE;
    uint32_t chunk = PAGE_SIZE - within;
    if (chunk > bytes - done) { chunk = bytes - done; }
    const uint8_t *src = (const uint8_t *)pmm_phys_to_virt(rx->pages[offset / PAGE_SIZE]) + within;
    int rc = copy(destination, src, chunk);
    if (rc) { return rc; }
    offset = (offset + chunk) % CELL_TCP_RX_CAP;
    done += chunk;
  }
  if (!(flags & MSG_PEEK)) {
    consume(file, bytes);
    tcp_ack_current_state(file);
  }
  return bytes;
}
