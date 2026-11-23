#include "drivers/virtio_console.h"

#include "arch/aarch64/platform.h"
#include "lib/mem.h"
#include "mm/pmm.h"

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
  VIRTIO_MMIO_STATUS = 0x070,
  VIRTIO_MMIO_QUEUE_DESC_LOW = 0x080,
  VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090,
  VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0a0,
  VIRTIO_MAGIC = 0x74726976,
  VIRTIO_DEVICE_CONSOLE = 3,
  VIRTIO_STATUS_ACKNOWLEDGE = 1,
  VIRTIO_STATUS_DRIVER = 2,
  VIRTIO_STATUS_DRIVER_OK = 4,
  VIRTIO_STATUS_FEATURES_OK = 8,
  VIRTIO_F_VERSION_1_BIT = 0,
  TX_QUEUE = 1,
  QUEUE_SIZE = 8,
  TX_BUFFER_SIZE = 256,
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

static uint64_t hhdm;
static uint64_t mmio_base;
static bool console_ready;
static uint16_t tx_avail_idx;
static uint16_t tx_used_idx;
static uint64_t desc_pa;
static uint64_t avail_pa;
static uint64_t used_pa;
static uint64_t tx_pa;
static struct virtq_desc *desc;
static struct virtq_avail *avail;
static volatile struct virtq_used *used;
static uint8_t *tx_buf;

static volatile uint32_t *reg32(uint64_t offset) {
  return (volatile uint32_t *)(uintptr_t)(hhdm + mmio_base + offset);
}

static uint32_t read32(uint64_t offset) {
  return *reg32(offset);
}

static void write32(uint64_t offset, uint32_t value) {
  *reg32(offset) = value;
  aarch64_io_barrier();
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

bool virtio_console_init(uint64_t hhdm_offset) {
  hhdm = hhdm_offset;
  console_ready = false;
  mmio_base = 0;

  for (uint32_t i = 0; i < VIRTIO_MMIO_SLOTS; ++i) {
    mmio_base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_SLOT_SIZE;
    if (read32(VIRTIO_MMIO_MAGIC) == VIRTIO_MAGIC && read32(VIRTIO_MMIO_VERSION) == 2 &&
        read32(VIRTIO_MMIO_DEVICE_ID) == VIRTIO_DEVICE_CONSOLE) {
      break;
    }
    mmio_base = 0;
  }
  if (mmio_base == 0) { return false; }

  write32(VIRTIO_MMIO_STATUS, 0);
  set_status(VIRTIO_STATUS_ACKNOWLEDGE);
  set_status(VIRTIO_STATUS_DRIVER);

  write32(VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
  uint32_t features_hi = read32(VIRTIO_MMIO_DEVICE_FEATURES);
  if ((features_hi & (1u << VIRTIO_F_VERSION_1_BIT)) == 0) { return false; }
  write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
  write32(VIRTIO_MMIO_DRIVER_FEATURES, 0);
  write32(VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
  write32(VIRTIO_MMIO_DRIVER_FEATURES, 1u << VIRTIO_F_VERSION_1_BIT);
  set_status(VIRTIO_STATUS_FEATURES_OK);
  if ((read32(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) { return false; }

  write32(VIRTIO_MMIO_QUEUE_SEL, TX_QUEUE);
  uint32_t qmax = read32(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if (qmax < QUEUE_SIZE) { return false; }
  write32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

  desc_pa = alloc_zero_page((void **)&desc);
  avail_pa = alloc_zero_page((void **)&avail);
  used_pa = alloc_zero_page((void **)&used);
  tx_pa = alloc_zero_page((void **)&tx_buf);
  if (desc_pa == 0 || avail_pa == 0 || used_pa == 0 || tx_pa == 0) { return false; }

  write_pa_pair(VIRTIO_MMIO_QUEUE_DESC_LOW, desc_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DRIVER_LOW, avail_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DEVICE_LOW, used_pa);
  write32(VIRTIO_MMIO_QUEUE_READY, 1);
  set_status(VIRTIO_STATUS_DRIVER_OK);

  tx_avail_idx = 0;
  tx_used_idx = used->idx;
  console_ready = true;
  return true;
}

bool virtio_console_ready(void) {
  return console_ready;
}

void virtio_console_putc(char c) {
  if (!console_ready) { return; }
  tx_buf[0] = (uint8_t)c;
  desc[0].addr = tx_pa;
  desc[0].len = 1;
  desc[0].flags = 0;
  desc[0].next = 0;
  avail->ring[tx_avail_idx % QUEUE_SIZE] = 0;
  aarch64_io_barrier();
  avail->idx = (uint16_t)(tx_avail_idx + 1u);
  tx_avail_idx = avail->idx;
  write32(VIRTIO_MMIO_QUEUE_NOTIFY, TX_QUEUE);

  for (uint32_t spin = 0; spin < 100000; ++spin) {
    aarch64_io_barrier();
    if (used->idx != tx_used_idx) {
      tx_used_idx = used->idx;
      return;
    }
  }
  console_ready = false;
}
