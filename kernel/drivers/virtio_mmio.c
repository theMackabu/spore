#include "virtio_mmio.h"
#include "lib/mem.h"
#include "mm/pmm.h"

bool virtio_matches(uintptr_t base, uint32_t device) {
  return virtio_read(base, VIRTIO_MMIO_MAGIC) == VIRTIO_MAGIC &&
         virtio_read(base, VIRTIO_MMIO_VERSION) == 2 && virtio_read(base, VIRTIO_MMIO_DEVICE_ID) == device;
}

uintptr_t virtio_find(uintptr_t base, unsigned slots, unsigned stride, uint32_t device) {
  for (unsigned i = 0; i < slots; ++i, base += stride) {
    if (virtio_matches(base, device)) { return base; }
  }
  return 0;
}

enum virtio_features virtio_negotiate(uintptr_t base) {
  virtio_write(base, VIRTIO_MMIO_STATUS, 0);
  virtio_set_status(base, VIRTIO_STATUS_ACKNOWLEDGE);
  virtio_set_status(base, VIRTIO_STATUS_DRIVER);
  virtio_write(base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
  uint32_t version = 1u << VIRTIO_F_VERSION_1_BIT;
  if (!(virtio_read(base, VIRTIO_MMIO_DEVICE_FEATURES) & version)) { return VIRTIO_VERSION_MISSING; }
  virtio_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
  virtio_write(base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
  virtio_write(base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
  virtio_write(base, VIRTIO_MMIO_DRIVER_FEATURES, version);
  virtio_set_status(base, VIRTIO_STATUS_FEATURES_OK);
  return (virtio_read(base, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) ? VIRTIO_FEATURES_OK : VIRTIO_FEATURES_REJECTED;
}

uint64_t virtio_alloc_page(uint64_t hhdm, void **virt) {
  uint64_t pa = pmm_alloc_zero_page();
  *virt = pa ? (void *)(uintptr_t)(hhdm + pa) : NULL;
  return pa;
}

bool virtq_alloc(struct virtq *q, uint64_t hhdm) {
  if (!q->desc_pa) { q->desc_pa = virtio_alloc_page(hhdm, (void **)&q->desc); }
  if (!q->avail_pa) { q->avail_pa = virtio_alloc_page(hhdm, (void **)&q->avail); }
  if (!q->used_pa) { q->used_pa = virtio_alloc_page(hhdm, (void **)&q->used); }
  return q->desc_pa && q->avail_pa && q->used_pa;
}

static void write_pa(uintptr_t base, unsigned reg, uint64_t pa) {
  virtio_write(base, reg, (uint32_t)pa);
  virtio_write(base, reg + 4, (uint32_t)(pa >> 32));
}

bool virtq_setup(uintptr_t base, unsigned index, unsigned size, struct virtq *q) {
  virtio_write(base, VIRTIO_MMIO_QUEUE_SEL, index);
  if (virtio_read(base, VIRTIO_MMIO_QUEUE_NUM_MAX) < size) { return false; }
  kmemset(q->desc, 0, PAGE_SIZE);
  kmemset(q->avail, 0, PAGE_SIZE);
  kmemset((void *)q->used, 0, PAGE_SIZE);
  virtio_write(base, VIRTIO_MMIO_QUEUE_NUM, size);
  write_pa(base, VIRTIO_MMIO_QUEUE_DESC_LOW, q->desc_pa);
  write_pa(base, VIRTIO_MMIO_QUEUE_DRIVER_LOW, q->avail_pa);
  write_pa(base, VIRTIO_MMIO_QUEUE_DEVICE_LOW, q->used_pa);
  virtio_write(base, VIRTIO_MMIO_QUEUE_READY, 1);
  q->avail_idx = 0;
  q->used_idx = q->used->idx;
  return true;
}
