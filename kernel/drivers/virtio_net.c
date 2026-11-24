#include "drivers/virtio_net.h"

#include "virtio_mmio.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "net/net.h"

#include <stddef.h>
#include <stdint.h>

enum {
  VIRTIO_DEVICE_NET = 1,
  RX_QUEUE = 0,
  TX_QUEUE = 1,
  QUEUE_SIZE = 128,
  NET_HDR_SIZE = 12,
  FRAME_BUFFER_SIZE = 2048,
};

static uint64_t hhdm;
static uint64_t mmio_base;
static bool net_ready;
static struct virtq rxq;
static struct virtq txq;
static uint64_t rx_pa[QUEUE_SIZE];
static uint8_t *rx_buf[QUEUE_SIZE];
static uint64_t tx_pa[QUEUE_SIZE];
static uint8_t *tx_buf[QUEUE_SIZE];
static bool tx_inflight[QUEUE_SIZE];
static uint32_t tx_len[QUEUE_SIZE];
static uint64_t tx_packets;
static uint64_t rx_packets;
static uint64_t tx_bytes;
static uint64_t rx_bytes;

static void rx_refill(uint16_t id) {
  rxq.desc[id].addr = rx_pa[id];
  rxq.desc[id].len = FRAME_BUFFER_SIZE;
  rxq.desc[id].flags = VIRTQ_DESC_F_WRITE;
  rxq.desc[id].next = 0;
  virtq_publish(&rxq, QUEUE_SIZE, id);
}

static void tx_reap(void) {
  while (txq.used->idx != txq.used_idx) {
    virtio_barrier();
    struct virtq_used_elem elem = txq.used->ring[txq.used_idx % QUEUE_SIZE];
    txq.used_idx++;
    if (elem.id >= QUEUE_SIZE || !tx_inflight[elem.id]) { continue; }
    tx_inflight[elem.id] = false;
    ++tx_packets;
    tx_bytes += tx_len[elem.id];
    tx_len[elem.id] = 0;
  }
}

static int tx_free_desc(void) {
  for (uint16_t i = 0; i < QUEUE_SIZE; ++i) {
    if (!tx_inflight[i]) { return (int)i; }
  }
  return -1;
}

constexpr unsigned FRAMES_PER_PAGE = PAGE_SIZE / FRAME_BUFFER_SIZE;
static_assert(PAGE_SIZE % FRAME_BUFFER_SIZE == 0 && QUEUE_SIZE % FRAMES_PER_PAGE == 0);

static bool allocate_frames(uint64_t *physical, uint8_t **buffers, unsigned id) {
  unsigned first = id - id % FRAMES_PER_PAGE;
  uint64_t page = pmm_alloc_page();
  if (!page) { return false; }
  for (unsigned i = 0; i < FRAMES_PER_PAGE; ++i) {
    physical[first + i] = page + i * FRAME_BUFFER_SIZE;
    buffers[first + i] = pmm_phys_to_virt(physical[first + i]);
  }
  return true;
}

bool virtio_net_init(uint64_t hhdm_offset) {
  hhdm = hhdm_offset;
  net_ready = false;
  mmio_base = virtio_find(hhdm + VIRTIO_MMIO_BASE, VIRTIO_MMIO_SLOTS, VIRTIO_MMIO_SLOT_SIZE, VIRTIO_DEVICE_NET);
  if (mmio_base == 0) {
    mmio_base = hhdm + VIRTIO_MMIO_BASE;
    kprintf("[spore] virtio-net: device not present magic=%x version=%u devid=%u\n",
            (unsigned)virtio_read(mmio_base, VIRTIO_MMIO_MAGIC), (unsigned)virtio_read(mmio_base, VIRTIO_MMIO_VERSION),
            (unsigned)virtio_read(mmio_base, VIRTIO_MMIO_DEVICE_ID));
    return false;
  }

  enum virtio_features features = virtio_negotiate(mmio_base);
  if (features != VIRTIO_FEATURES_OK) {
    kprintf("[spore] virtio-net: %s\n", features == VIRTIO_VERSION_MISSING ? "VERSION_1 missing" : "features rejected");
    return false;
  }

  if (!virtq_alloc(&rxq, hhdm) || !virtq_alloc(&txq, hhdm)) {
    kprintf("[spore] virtio-net: queue allocation failed\n");
    return false;
  }
  for (unsigned i = 0; i < QUEUE_SIZE; i += FRAMES_PER_PAGE) {
    if ((!rx_pa[i] && !allocate_frames(rx_pa, rx_buf, i)) || (!tx_pa[i] && !allocate_frames(tx_pa, tx_buf, i))) {
      kprintf("[spore] virtio-net: frame buffer allocation failed\n");
      return false;
    }
  }
  if (!virtq_setup(mmio_base, RX_QUEUE, QUEUE_SIZE, &rxq) || !virtq_setup(mmio_base, TX_QUEUE, QUEUE_SIZE, &txq)) {
    kprintf("[spore] virtio-net: queue too small\n");
    return false;
  }
  for (uint16_t i = 0; i < QUEUE_SIZE; ++i) {
    rx_refill(i);
  }
  virtio_write(mmio_base, VIRTIO_MMIO_QUEUE_NOTIFY, RX_QUEUE);

  virtio_set_status(mmio_base, VIRTIO_STATUS_DRIVER_OK);
  net_ready = true;
  tx_packets = 0;
  rx_packets = 0;
  tx_bytes = 0;
  rx_bytes = 0;
  for (uint16_t i = 0; i < QUEUE_SIZE; ++i) {
    tx_inflight[i] = false;
    tx_len[i] = 0;
  }
  kprintf("[spore] virtio-net: mmio %p up rxq=%u txq=%u\n", (void *)(uintptr_t)(mmio_base - hhdm), (unsigned)QUEUE_SIZE,
          (unsigned)QUEUE_SIZE);
  return true;
}

void virtio_net_poll(void) {
  if (!net_ready) { return; }
  uint32_t isr = virtio_read(mmio_base, VIRTIO_MMIO_INTERRUPT_STATUS);
  if (isr != 0) { virtio_write(mmio_base, VIRTIO_MMIO_INTERRUPT_ACK, isr); }
  tx_reap();
  for (;;) {
    virtio_barrier();
    if (rxq.used->idx == rxq.used_idx) { break; }
    struct virtq_used_elem elem = rxq.used->ring[rxq.used_idx % QUEUE_SIZE];
    rxq.used_idx++;
    if (elem.id < QUEUE_SIZE && elem.len > NET_HDR_SIZE) {
      uint32_t frame_len = elem.len - NET_HDR_SIZE;
      ++rx_packets;
      rx_bytes += frame_len;
      net_receive_ethernet(rx_buf[elem.id] + NET_HDR_SIZE, frame_len);
      rx_refill((uint16_t)elem.id);
    }
  }
  virtio_write(mmio_base, VIRTIO_MMIO_QUEUE_NOTIFY, RX_QUEUE);
}

bool virtio_net_send_frame(const void *frame, uint32_t len) {
  if (!net_ready || len + NET_HDR_SIZE > FRAME_BUFFER_SIZE) { return false; }
  tx_reap();
  int id = tx_free_desc();
  if (id < 0) {
    for (uint32_t spin = 0; spin < 100000; ++spin) {
      virtio_barrier();
      tx_reap();
      id = tx_free_desc();
      if (id >= 0) { break; }
    }
  }
  if (id < 0) { return false; }

  kmemset(tx_buf[id], 0, NET_HDR_SIZE);
  kmemcpy(tx_buf[id] + NET_HDR_SIZE, frame, len);
  txq.desc[id].addr = tx_pa[id];
  txq.desc[id].len = len + NET_HDR_SIZE;
  txq.desc[id].flags = 0;
  txq.desc[id].next = 0;
  tx_inflight[id] = true;
  tx_len[id] = len;
  virtq_publish(&txq, QUEUE_SIZE, id);
  virtio_write(mmio_base, VIRTIO_MMIO_QUEUE_NOTIFY, TX_QUEUE);
  return true;
}

struct virtio_net_stats virtio_net_stats(void) {
  return (struct virtio_net_stats){
    .rx_bytes = rx_bytes,
    .rx_packets = rx_packets,
    .tx_bytes = tx_bytes,
    .tx_packets = tx_packets,
  };
}
