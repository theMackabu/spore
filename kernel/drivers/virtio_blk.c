#include "drivers/virtio_blk.h"

#include "virtio_mmio.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "mm/pmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  VIRTIO_DEVICE_BLK = 2,
  QUEUE_SIZE = 8,
  VIRTIO_BLK_T_IN = 0,
  VIRTIO_BLK_T_OUT = 1,
  SECTOR_SIZE = 512,
  MAX_IO_SIZE = 128 * 1024,
  IO_BUFFER_PAGES = MAX_IO_SIZE / 4096,
};

struct virtio_blk_req {
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
};

static uint64_t hhdm;
static uint64_t mmio_base;
static uint64_t root_base;
static uint64_t boot_base;
static uint64_t active_base;
static bool blk_ready;
static struct virtq queue;
static uint64_t req_pa;
static struct virtio_blk_req *req;
static uint64_t sector_pa;
static uint8_t *sector_buf;
static uint64_t status_pa;
static volatile uint8_t *status_buf;

static uint64_t alloc_zero_contiguous(uint64_t pages, void **virt) {
  uint64_t pa = pmm_alloc_contiguous_pages(pages);
  if (pa == 0) {
    *virt = NULL;
    return 0;
  }
  *virt = (void *)(uintptr_t)(hhdm + pa);
  kmemset(*virt, 0, pages * 4096);
  return pa;
}

static bool setup_device(uint64_t base) {
  mmio_base = base;
  active_base = base;
  if (virtio_negotiate(mmio_base) != VIRTIO_FEATURES_OK || !virtq_alloc(&queue, hhdm)) { return false; }
  if (req_pa == 0) { req_pa = virtio_alloc_page(hhdm, (void **)&req); }
  if (sector_pa == 0) { sector_pa = alloc_zero_contiguous(IO_BUFFER_PAGES, (void **)&sector_buf); }
  if (status_pa == 0) { status_pa = virtio_alloc_page(hhdm, (void **)&status_buf); }
  if (req_pa == 0 || sector_pa == 0 || status_pa == 0) {
    return false;
  }
  kmemset(req, 0, 4096);
  kmemset(sector_buf, 0, MAX_IO_SIZE);
  *(uint8_t *)status_buf = 0;

  if (!virtq_setup(mmio_base, 0, QUEUE_SIZE, &queue)) { return false; }
  virtio_set_status(mmio_base, VIRTIO_STATUS_DRIVER_OK);
  blk_ready = true;
  return true;
}

static bool select_device(uint64_t base) {
  if (base == 0) { return false; }
  if (blk_ready && active_base == base) { return true; }
  blk_ready = false;
  return setup_device(base);
}

static bool rw_sectors(uint64_t sector, void *buf, uint32_t bytes, bool write) {
  if (!blk_ready || bytes == 0 || bytes > MAX_IO_SIZE || (bytes % SECTOR_SIZE) != 0) { return false; }

  if (write && buf != sector_buf) { kmemcpy(sector_buf, buf, bytes); }
  *req = (struct virtio_blk_req){.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN, .reserved = 0, .sector = sector};
  *status_buf = 0xff;

  queue.desc[0].addr = req_pa;
  queue.desc[0].len = sizeof(*req);
  queue.desc[0].flags = VIRTQ_DESC_F_NEXT;
  queue.desc[0].next = 1;
  queue.desc[1].addr = sector_pa;
  queue.desc[1].len = bytes;
  queue.desc[1].flags = write ? VIRTQ_DESC_F_NEXT : (VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE);
  queue.desc[1].next = 2;
  queue.desc[2].addr = status_pa;
  queue.desc[2].len = 1;
  queue.desc[2].flags = VIRTQ_DESC_F_WRITE;
  queue.desc[2].next = 0;

  virtq_publish(&queue, QUEUE_SIZE, 0);
  virtio_write(mmio_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

  for (;;) {
    virtio_barrier();
    if (queue.used->idx != queue.used_idx) {
      ++queue.used_idx;
      if (*status_buf != 0) { return false; }
      if (!write && buf != sector_buf) { kmemcpy(buf, sector_buf, bytes); }
      uint32_t isr = virtio_read(mmio_base, VIRTIO_MMIO_INTERRUPT_STATUS);
      if (isr != 0) { virtio_write(mmio_base, VIRTIO_MMIO_INTERRUPT_ACK, isr); }
      return true;
    }
  }
}

static bool transfer_active(uint64_t offset, void *buffer, uint32_t len, bool write) {
  uint8_t *cursor = buffer;
  while (len) {
    uint64_t sector = offset / SECTOR_SIZE;
    uint32_t within = offset % SECTOR_SIZE;
    uint32_t chunk;
    if (!within && len >= SECTOR_SIZE) {
      chunk = len > MAX_IO_SIZE ? MAX_IO_SIZE : len;
      chunk -= chunk % SECTOR_SIZE;
      if (!rw_sectors(sector, cursor, chunk, write)) { return false; }
    } else {
      chunk = SECTOR_SIZE - within;
      if (chunk > len) { chunk = len; }
      if (!rw_sectors(sector, sector_buf, SECTOR_SIZE, false)) { return false; }
      if (write) {
        kmemcpy(sector_buf + within, cursor, chunk);
        if (!rw_sectors(sector, sector_buf, SECTOR_SIZE, true)) { return false; }
      } else {
        kmemcpy(cursor, sector_buf + within, chunk);
      }
    }
    cursor += chunk;
    offset += chunk;
    len -= chunk;
  }
  return true;
}

bool virtio_blk_read(uint64_t offset, void *dst, uint32_t len) {
  return select_device(root_base) && transfer_active(offset, dst, len, false);
}

bool virtio_blk_write(uint64_t offset, const void *src, uint32_t len) {
  return select_device(root_base) && transfer_active(offset, (void *)src, len, true);
}

bool virtio_blk_read_boot(uint64_t offset, void *dst, uint32_t len) {
  if (!select_device(boot_base)) { return false; }
  bool ok = transfer_active(offset, dst, len, false);
  return select_device(root_base) && ok;
}

bool virtio_blk_init(uint64_t hhdm_offset) {
  hhdm = hhdm_offset;
  blk_ready = false;
  root_base = 0;
  boot_base = 0;
  active_base = 0;
  for (uint32_t i = 0; i < VIRTIO_MMIO_SLOTS; ++i) {
    uintptr_t base = virtio_slot(hhdm, i, VIRTIO_DEVICE_BLK);
    if (base) {
      if (!setup_device(base)) {
        blk_ready = false;
        continue;
      }
      uint8_t super[128];
      if (!transfer_active(1024, super, sizeof(super), false)) {
        blk_ready = false;
        continue;
      }
      uint16_t magic = (uint16_t)super[56] | ((uint16_t)super[57] << 8);
      if (magic == 0xef53u) {
        root_base = base;
        kprintf("[spore] virtio-blk: mmio %p ext2 magic 0x%x\n", (void *)(uintptr_t)root_base, (unsigned)magic);
        return select_device(root_base);
      }
      if (boot_base == 0) { boot_base = base; }
      kprintf("[spore] virtio-blk: mmio %p skipped magic 0x%x\n", (void *)(uintptr_t)base, (unsigned)magic);
      blk_ready = false;
    }
  }
  if (root_base == 0) {
    kprintf("[spore] virtio-blk: root disk not present\n");
    return false;
  }
  return false;
}
