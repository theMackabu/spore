#include "virtio_blk.h"

#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"

#include <stdbool.h>
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
  VIRTIO_DEVICE_BLK = 2,
  VIRTIO_STATUS_ACKNOWLEDGE = 1,
  VIRTIO_STATUS_DRIVER = 2,
  VIRTIO_STATUS_DRIVER_OK = 4,
  VIRTIO_STATUS_FEATURES_OK = 8,
  VIRTIO_F_VERSION_1_BIT = 0,
  QUEUE_SIZE = 8,
  VIRTQ_DESC_F_NEXT = 1,
  VIRTQ_DESC_F_WRITE = 2,
  VIRTIO_BLK_T_IN = 0,
  VIRTIO_BLK_T_OUT = 1,
  SECTOR_SIZE = 512,
  MAX_IO_SIZE = 128 * 1024,
  IO_BUFFER_PAGES = MAX_IO_SIZE / 4096,
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
static uint64_t desc_pa;
static uint64_t avail_pa;
static uint64_t used_pa;
static struct virtq_desc *desc;
static struct virtq_avail *avail;
static volatile struct virtq_used *used;
static uint16_t avail_idx;
static uint16_t used_idx;
static uint64_t req_pa;
static struct virtio_blk_req *req;
static uint64_t sector_pa;
static uint8_t *sector_buf;
static uint64_t status_pa;
static volatile uint8_t *status_buf;

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

static void write_pa_pair(uint64_t low_reg, uint64_t pa) {
  write32(low_reg, (uint32_t)pa);
  write32(low_reg + 4, (uint32_t)(pa >> 32));
}

static bool setup_device(uint64_t base) {
  mmio_base = base;
  active_base = base;
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

  if (desc_pa == 0) { desc_pa = alloc_zero_page((void **)&desc); }
  if (avail_pa == 0) { avail_pa = alloc_zero_page((void **)&avail); }
  if (used_pa == 0) { used_pa = alloc_zero_page((void **)&used); }
  if (req_pa == 0) { req_pa = alloc_zero_page((void **)&req); }
  if (sector_pa == 0) { sector_pa = alloc_zero_contiguous(IO_BUFFER_PAGES, (void **)&sector_buf); }
  if (status_pa == 0) { status_pa = alloc_zero_page((void **)&status_buf); }
  if (desc_pa == 0 || avail_pa == 0 || used_pa == 0 || req_pa == 0 || sector_pa == 0 || status_pa == 0) {
    return false;
  }
  kmemset(desc, 0, 4096);
  kmemset(avail, 0, 4096);
  kmemset((void *)used, 0, 4096);
  kmemset(req, 0, 4096);
  kmemset(sector_buf, 0, MAX_IO_SIZE);
  *(uint8_t *)status_buf = 0;

  write32(VIRTIO_MMIO_QUEUE_SEL, 0);
  if (read32(VIRTIO_MMIO_QUEUE_NUM_MAX) < QUEUE_SIZE) { return false; }
  write32(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DESC_LOW, desc_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DRIVER_LOW, avail_pa);
  write_pa_pair(VIRTIO_MMIO_QUEUE_DEVICE_LOW, used_pa);
  write32(VIRTIO_MMIO_QUEUE_READY, 1);
  avail_idx = 0;
  used_idx = used->idx;

  set_status(VIRTIO_STATUS_DRIVER_OK);
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

  if (write) { kmemcpy(sector_buf, buf, bytes); }
  *req = (struct virtio_blk_req){.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN, .reserved = 0, .sector = sector};
  *status_buf = 0xff;

  desc[0].addr = req_pa;
  desc[0].len = sizeof(*req);
  desc[0].flags = VIRTQ_DESC_F_NEXT;
  desc[0].next = 1;
  desc[1].addr = sector_pa;
  desc[1].len = bytes;
  desc[1].flags = write ? VIRTQ_DESC_F_NEXT : (VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE);
  desc[1].next = 2;
  desc[2].addr = status_pa;
  desc[2].len = 1;
  desc[2].flags = VIRTQ_DESC_F_WRITE;
  desc[2].next = 0;

  avail->ring[avail_idx % QUEUE_SIZE] = 0;
  __asm__ volatile("dsb sy" : : : "memory");
  avail->idx = (uint16_t)(avail_idx + 1u);
  avail_idx = avail->idx;
  write32(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

  for (;;) {
    __asm__ volatile("dsb sy" : : : "memory");
    if (used->idx != used_idx) {
      ++used_idx;
      if (*status_buf != 0) { return false; }
      if (!write) { kmemcpy(buf, sector_buf, bytes); }
      uint32_t isr = read32(VIRTIO_MMIO_INTERRUPT_STATUS);
      if (isr != 0) { write32(VIRTIO_MMIO_INTERRUPT_ACK, isr); }
      return true;
    }
  }
}

static bool read_sector(uint64_t sector, void *dst) {
  return rw_sectors(sector, dst, SECTOR_SIZE, false);
}

static bool write_sector(uint64_t sector, const void *src) {
  return rw_sectors(sector, (void *)src, SECTOR_SIZE, true);
}

static bool read_active(uint64_t offset, void *dst, uint32_t len) {
  uint8_t *out = dst;
  while (len > 0) {
    uint64_t sector = offset / SECTOR_SIZE;
    uint32_t within = (uint32_t)(offset % SECTOR_SIZE);
    if (within == 0 && len >= SECTOR_SIZE) {
      uint32_t chunk = len > MAX_IO_SIZE ? MAX_IO_SIZE : len;
      chunk -= chunk % SECTOR_SIZE;
      if (!rw_sectors(sector, out, chunk, false)) { return false; }
      out += chunk;
      offset += chunk;
      len -= chunk;
      continue;
    }
    uint32_t chunk = SECTOR_SIZE - within;
    if (chunk > len) { chunk = len; }
    if (!read_sector(sector, sector_buf)) { return false; }
    kmemcpy(out, sector_buf + within, chunk);
    out += chunk;
    offset += chunk;
    len -= chunk;
  }
  return true;
}

bool virtio_blk_read(uint64_t offset, void *dst, uint32_t len) {
  return select_device(root_base) && read_active(offset, dst, len);
}

static bool write_active(uint64_t offset, const void *src, uint32_t len) {
  const uint8_t *in = src;
  while (len > 0) {
    uint64_t sector = offset / SECTOR_SIZE;
    uint32_t within = (uint32_t)(offset % SECTOR_SIZE);
    if (within == 0 && len >= SECTOR_SIZE) {
      uint32_t chunk = len > MAX_IO_SIZE ? MAX_IO_SIZE : len;
      chunk -= chunk % SECTOR_SIZE;
      if (!rw_sectors(sector, (void *)in, chunk, true)) { return false; }
      in += chunk;
      offset += chunk;
      len -= chunk;
      continue;
    }
    uint32_t chunk = SECTOR_SIZE - within;
    if (chunk > len) { chunk = len; }
    if (within != 0 || chunk != SECTOR_SIZE) {
      if (!read_sector(sector, sector_buf)) { return false; }
    }
    kmemcpy(sector_buf + within, in, chunk);
    if (!write_sector(sector, sector_buf)) { return false; }
    in += chunk;
    offset += chunk;
    len -= chunk;
  }
  return true;
}

bool virtio_blk_write(uint64_t offset, const void *src, uint32_t len) {
  return select_device(root_base) && write_active(offset, src, len);
}

bool virtio_blk_read_boot(uint64_t offset, void *dst, uint32_t len) {
  if (!select_device(boot_base)) { return false; }
  bool ok = read_active(offset, dst, len);
  return select_device(root_base) && ok;
}

bool virtio_blk_init(uint64_t hhdm_offset) {
  hhdm = hhdm_offset;
  blk_ready = false;
  root_base = 0;
  boot_base = 0;
  active_base = 0;
  for (uint32_t i = 0; i < VIRTIO_MMIO_SLOTS; ++i) {
    uint64_t base = VIRTIO_MMIO_BASE + (uint64_t)i * VIRTIO_MMIO_SLOT_SIZE;
    mmio_base = base;
    if (read32(VIRTIO_MMIO_MAGIC) == VIRTIO_MAGIC && read32(VIRTIO_MMIO_VERSION) == 2 &&
        read32(VIRTIO_MMIO_DEVICE_ID) == VIRTIO_DEVICE_BLK) {
      if (!setup_device(base)) {
        blk_ready = false;
        continue;
      }
      uint8_t super[128];
      if (!read_active(1024, super, sizeof(super))) {
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
