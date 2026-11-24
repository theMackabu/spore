#include "drivers/virtio_console.h"

#include "virtio_mmio.h"
#include "lib/mem.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

enum {
  VIRTIO_DEVICE_CONSOLE = 3,
  TX_QUEUE = 1,
  QUEUE_SIZE = 8,
};

static uint64_t hhdm;
static uint64_t mmio_base;
static bool console_ready;
static struct virtq txq;
static uint64_t tx_pa;
static uint8_t *tx_buf;

bool virtio_console_init_at(uint64_t hhdm_offset, uintptr_t base, unsigned slots, unsigned stride) {
  hhdm = hhdm_offset;
  console_ready = false;
  mmio_base = virtio_find(base, slots, stride, VIRTIO_DEVICE_CONSOLE);
  if (!mmio_base || virtio_negotiate(mmio_base) != VIRTIO_FEATURES_OK) { return false; }
  if (!virtq_alloc(&txq, hhdm)) { return false; }
  tx_pa = virtio_alloc_page(hhdm, (void **)&tx_buf);
  if (!tx_pa || !virtq_setup(mmio_base, TX_QUEUE, QUEUE_SIZE, &txq)) { return false; }
  virtio_set_status(mmio_base, VIRTIO_STATUS_DRIVER_OK);

  console_ready = true;
  return true;
}

bool virtio_console_init(uint64_t hhdm_offset) {
  return virtio_console_init_at(hhdm_offset, hhdm_offset + VIRTIO_MMIO_BASE, VIRTIO_MMIO_SLOTS, VIRTIO_MMIO_SLOT_SIZE);
}

bool virtio_console_ready(void) {
  return console_ready;
}

void virtio_console_write(const uint8_t *data, size_t len) {
  if (!console_ready || !len || len > 4096) { return; }
  for (size_t i = 0; i < len; ++i) { tx_buf[i] = data[i]; }
  txq.desc[0].addr = tx_pa;
  txq.desc[0].len = len;
  txq.desc[0].flags = 0;
  txq.desc[0].next = 0;
  virtq_publish(&txq, QUEUE_SIZE, 0);
  virtio_write(mmio_base, VIRTIO_MMIO_QUEUE_NOTIFY, TX_QUEUE);

  for (uint32_t spin = 0; spin < 100000; ++spin) {
    virtio_barrier();
    if (txq.used->idx != txq.used_idx) {
      txq.used_idx = txq.used->idx;
      return;
    }
  }
  console_ready = false;
}
