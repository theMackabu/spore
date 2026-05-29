#include "virtio_net.h"

#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "net.h"

#include <stddef.h>
#include <stdint.h>

enum {
  VIRTIO_MMIO_BASE = 0x0a000000,
  VIRTIO_MMIO_SLOT_SIZE = 0x200,
  VIRTIO_MMIO_SLOTS = 32,
  VIRTIO_MMIO_MAGIC = 0x000,
  VIRTIO_MMIO_VERSION = 0x004,
  VIRTIO_MMIO_DEVICE_ID = 0x008,
  VIRTIO_MMIO_DEVICE_FEATURES = 0x010,
  VIRTIO_MMIO_DEVICE_FEATURES_SEL = 0x014,
  VIRTIO_MMIO_DRIVER_FEATURES = 0x020,
  VIRTIO_MMIO_DRIVER_FEATURES_SEL = 0x024,
  VIRTIO_MMIO_QUEUE_SEL = 0x030,
  VIRTIO_MMIO_QUEUE_NUM_MAX = 0x034,
  VIRTIO_MMIO_QUEUE_NUM = 0x038,
  VIRTIO_MMIO_QUEUE_READY = 0x044,
  VIRTIO_MMIO_QUEUE_NOTIFY = 0x050,
  VIRTIO_MMIO_INTERRUPT_STATUS = 0x060,
  VIRTIO_MMIO_INTERRUPT_ACK = 0x064,
  VIRTIO_MMIO_STATUS = 0x070,
  VIRTIO_MMIO_QUEUE_DESC_LOW = 0x080,
  VIRTIO_MMIO_QUEUE_DESC_HIGH = 0x084,
  VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090,
  VIRTIO_MMIO_QUEUE_DRIVER_HIGH = 0x094,
  VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0a0,
  VIRTIO_MMIO_QUEUE_DEVICE_HIGH = 0x0a4,
  VIRTIO_MAGIC = 0x74726976,
  VIRTIO_DEVICE_NET = 1,
  VIRTIO_STATUS_ACKNOWLEDGE = 1,
  VIRTIO_STATUS_DRIVER = 2,
  VIRTIO_STATUS_DRIVER_OK = 4,
  VIRTIO_STATUS_FEATURES_OK = 8,
  VIRTIO_F_VERSION_1_BIT = 0,
  RX_QUEUE = 0,
  TX_QUEUE = 1,
  QUEUE_SIZE = 64,
  NET_HDR_SIZE = 12,
  FRAME_BUFFER_SIZE = 2048,
  VIRTQ_DESC_F_NEXT = 1,
  VIRTQ_DESC_F_WRITE = 2,
};

struct virtq_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct virtq_avail {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[QUEUE_SIZE];
  uint16_t used_event;
};

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
};

struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[QUEUE_SIZE];
  uint16_t avail_event;
};

struct virtq {
  uint64_t desc_pa;
  uint64_t avail_pa;
  uint64_t used_pa;
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  volatile struct virtq_used *used;
  uint16_t avail_idx;
  uint16_t used_idx;
};

static uint64_t hhdm;
static uint64_t mmio_base;
static bool net_ready;
static struct virtq rxq;
static struct virtq txq;
static uint64_t rx_pa[QUEUE_SIZE];
static uint8_t *rx_buf[QUEUE_SIZE];
static uint64_t tx_pa;
static uint8_t *tx_buf;
static uint64_t tx_packets;
static uint64_t rx_packets;
static uint64_t tx_bytes;
static uint64_t rx_bytes;

static volatile uint32_t *reg32(uint64_t offset) {
  return (volatile uint32_t *)(uintptr_t)(hhdm + mmio_base + offset);
}

static uint32_t read32(uint64_t offset) {
  return *reg32(offset);
}

static void write32(uint64_t offset, uint32_t value) {
  *reg32(offset) = value;
  __asm__ volatile("dsb sy" : : : "memory");
}

static void set_status(uint32_t bits) {
  write32(VIRTIO_MMIO_STATUS, read32(VIRTIO_MMIO_STATUS) | bits);
}

static uint64_t alloc_zero_page(void **virt) {
  uint64_t pa = pmm_alloc_zero_page();
  if (pa == 0) {
    *virt = NULL;
    return 0;
  }
  *virt = (void *)(uintptr_t)(hhdm + pa);
  return pa;
}

static void write_pa_pair(uint64_t low_reg, uint64_t pa) {
  write32(low_reg, (uint32_t)pa);
  write32(low_reg + 4, (uint32_t)(pa >> 32));
}

static bool alloc_queue(struct virtq *q) {
  q->desc_pa = alloc_zero_page((void **)&q->desc);
  q->avail_pa = alloc_zero_page((void **)&q->avail);
  q->used_pa = alloc_zero_page((void **)&q->used);
  return q->desc_pa != 0 && q->avail_pa != 0 && q->used_pa != 0;
}

static bool setup_queue(uint32_t index, struct virtq *q) {
  write32(VIRTIO_MMIO_QUEUE_SEL, index);
  uint32_t qmax = read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if (qmax < QUEUE_SIZE) { return false; }
  write32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DESC_LOW, q->desc_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DRIVER_LOW, q->avail_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DEVICE_LOW, q->used_pa);
  write32(VIRTIO_MMIO_QUEUE_READY, 1);
  q->avail_idx = 0;
  q->used_idx = q->used->idx;
  return true;
}

static void rx_refill(uint16_t id) {
  rxq.desc[id].addr = rx_pa[id];
  rxq.desc[id].len = FRAME_BUFFER_SIZE;
  rxq.desc[id].flags = VIRTQ_DESC_F_WRITE;
  rxq.desc[id].next = 0;
  rxq.avail->ring[rxq.avail_idx % QUEUE_SIZE] = id;
  __asm__ volatile("dsb sy" : : : "memory");
  rxq.avail->idx = (uint16_t)(rxq.avail_idx + 1u);
  rxq.avail_idx = rxq.avail->idx;
}

bool virtio_net_init(uint64_t hhdm_offset) {
  hhdm = hhdm_offset;
  net_ready = false;
  mmio_base = 0;

  for (uint32_t i = 0; i < VIRTIO_MMIO_SLOTS; ++i) {
    mmio_base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_SLOT_SIZE;
    if (read32(VIRTIO_MMIO_MAGIC) == VIRTIO_MAGIC && read32(VIRTIO_MMIO_VERSION) == 2 &&
        read32(VIRTIO_MMIO_DEVICE_ID) == VIRTIO_DEVICE_NET) {
      break;
    }
    mmio_base = 0;
  }
  if (mmio_base == 0) {
    mmio_base = VIRTIO_MMIO_BASE;
    kprintf("[spore] virtio-net: device not present magic=%x version=%u devid=%u\n",
            (unsigned)read32(VIRTIO_MMIO_MAGIC), (unsigned)read32(VIRTIO_MMIO_VERSION),
            (unsigned)read32(VIRTIO_MMIO_DEVICE_ID));
    return false;
  }

  write32(VIRTIO_MMIO_STATUS, 0);
  set_status(VIRTIO_STATUS_ACKNOWLEDGE);
  set_status(VIRTIO_STATUS_DRIVER);

  write32(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
  uint32_t features_hi = read32(VIRTIO_MMIO_DEVICE_FEATURES);
  if ((features_hi & (1u << VIRTIO_F_VERSION_1_BIT)) == 0) {
    kprintf("[spore] virtio-net: VERSION_1 missing\n");
    return false;
  }
  write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
  write32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
  write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
  write32(VIRTIO_MMIO_DRIVER_FEATURES, 1u << VIRTIO_F_VERSION_1_BIT);
  set_status(VIRTIO_STATUS_FEATURES_OK);
  if ((read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
    kprintf("[spore] virtio-net: features rejected\n");
    return false;
  }

  if (!alloc_queue(&rxq) || !alloc_queue(&txq)) {
    kprintf("[spore] virtio-net: queue allocation failed\n");
    return false;
  }
  for (uint16_t i = 0; i < QUEUE_SIZE; ++i) {
    rx_pa[i] = alloc_zero_page((void **)&rx_buf[i]);
    if (rx_pa[i] == 0) {
      kprintf("[spore] virtio-net: rx buffer allocation failed\n");
      return false;
    }
  }
  tx_pa = alloc_zero_page((void **)&tx_buf);
  if (tx_pa == 0) {
    kprintf("[spore] virtio-net: tx buffer allocation failed\n");
    return false;
  }

  if (!setup_queue(RX_QUEUE, &rxq) || !setup_queue(TX_QUEUE, &txq)) {
    kprintf("[spore] virtio-net: queue too small\n");
    return false;
  }
  for (uint16_t i = 0; i < QUEUE_SIZE; ++i) {
    rx_refill(i);
  }
  write32(VIRTIO_MMIO_QUEUE_NOTIFY, RX_QUEUE);

  set_status(VIRTIO_STATUS_DRIVER_OK);
  net_ready = true;
  tx_packets = 0;
  rx_packets = 0;
  tx_bytes = 0;
  rx_bytes = 0;
  kprintf("[spore] virtio-net: mmio %p up rxq=%u txq=%u\n", (void *)(uintptr_t)mmio_base, (unsigned)QUEUE_SIZE,
          (unsigned)QUEUE_SIZE);
  return true;
}

void virtio_net_poll(void) {
  if (!net_ready) { return; }
  uint32_t isr = read32(VIRTIO_MMIO_INTERRUPT_STATUS);
  if (isr != 0) { write32(VIRTIO_MMIO_INTERRUPT_ACK, isr); }
  for (;;) {
    __asm__ volatile("dsb sy" : : : "memory");
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
  write32(VIRTIO_MMIO_QUEUE_NOTIFY, RX_QUEUE);
}

bool virtio_net_send_frame(const void *frame, uint32_t len) {
  if (!net_ready || len + NET_HDR_SIZE > FRAME_BUFFER_SIZE) { return false; }
  while (txq.used->idx != txq.used_idx) {
    txq.used_idx++;
  }
  kmemset(tx_buf, 0, NET_HDR_SIZE);
  kmemcpy(tx_buf + NET_HDR_SIZE, frame, len);
  txq.desc[0].addr = tx_pa;
  txq.desc[0].len = len + NET_HDR_SIZE;
  txq.desc[0].flags = 0;
  txq.desc[0].next = 0;
  txq.avail->ring[txq.avail_idx % QUEUE_SIZE] = 0;
  __asm__ volatile("dsb sy" : : : "memory");
  txq.avail->idx = (uint16_t)(txq.avail_idx + 1u);
  txq.avail_idx = txq.avail->idx;
  write32(VIRTIO_MMIO_QUEUE_NOTIFY, TX_QUEUE);
  for (uint32_t spin = 0; spin < 1000000; ++spin) {
    __asm__ volatile("dsb sy" : : : "memory");
    if (txq.used->idx != txq.used_idx) {
      txq.used_idx++;
      ++tx_packets;
      tx_bytes += len;
      return true;
    }
  }
  return false;
}

bool virtio_net_smoke_tx(void) {
  uint8_t frame[14 + 15];
  for (size_t i = 0; i < 6; ++i) {
    frame[i] = 0xff;
  }
  static const uint8_t src[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  kmemcpy(frame + 6, src, sizeof(src));
  frame[12] = 0x88;
  frame[13] = 0xb5;
  kmemcpy(frame + 14, "spore-net-smoke", 15);
  bool ok = virtio_net_send_frame(frame, sizeof(frame));
  kprintf("[spore] virtio-net: tx smoke %s len=%u\n", ok ? "PASS" : "TIMEOUT",
          (unsigned)(sizeof(frame) + NET_HDR_SIZE));
  return ok;
}

struct virtio_net_stats virtio_net_stats(void) {
  return (struct virtio_net_stats){
    .rx_bytes = rx_bytes,
    .rx_packets = rx_packets,
    .tx_bytes = tx_bytes,
    .tx_packets = tx_packets,
  };
}
