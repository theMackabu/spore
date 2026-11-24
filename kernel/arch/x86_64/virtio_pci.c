#include "arch/bringup/user.h"
#include "drivers/virtio_mmio.h"

struct pci_virtio {
  uintptr_t common, notify, isr;
  uint32_t notify_stride, notify_size;
  uint16_t notify_offsets[2];
  unsigned type;
};
static struct pci_virtio slots[32];
static uintptr_t next_mmio = 0xffffd00000000000ull;

static uint32_t cfg(unsigned dev, unsigned off) {
  uint32_t value = 0x80000000u | (dev << 11) | (off & 0xfc);
  __asm__ volatile("outl %0,%w1" ::"a"(value), "d"(0xcf8));
  __asm__ volatile("inl %w1,%0" : "=a"(value) : "d"(0xcfc));
  return value;
}
static uint8_t byte(unsigned dev, unsigned off) {
  return cfg(dev, off) >> (8 * (off & 3));
}
static uintptr_t map_cap(unsigned dev, unsigned cap, unsigned size) {
  unsigned bar = byte(dev, cap + 4);
  if (bar >= 6 || !size || size > 0x10000) { return 0; }
  uint32_t low = cfg(dev, 0x10 + bar * 4);
  if (!low || (low & 1)) { return 0; }
  uint64_t pa = low & ~15u;
  if ((low & 6) == 4 && bar < 5) { pa |= (uint64_t)cfg(dev, 0x14 + bar * 4) << 32; }
  pa += cfg(dev, cap + 8);
  uintptr_t start = next_mmio;
  unsigned offset = pa & (PAGE_SIZE - 1);
  for (unsigned bytes = 0; bytes < offset + size; bytes += PAGE_SIZE) {
    if (!map_page(next_mmio, (pa & ~4095ull) + bytes, PAGE_READ | PAGE_WRITE | PAGE_DEVICE)) { return 0; }
    next_mmio += PAGE_SIZE;
  }
  return start + offset;
}
void virtio_pci_init(void) {
  for (unsigned dev = 0; dev < 32; ++dev) {
    uint32_t id = cfg(dev, 0);
    if ((uint16_t)id != 0x1af4 || !(cfg(dev, 4) & (1u << 20))) { continue; }
    unsigned cap = byte(dev, 0x34);
    struct pci_virtio *v = &slots[dev];
    for (unsigned i = 0; cap >= 0x40 && cap <= 0xec && i < 48; ++i) {
      if (byte(dev, cap) == 9) {
        unsigned type = byte(dev, cap + 3), size = cfg(dev, cap + 12);
        if (type == 1 && size >= 56) { v->common = map_cap(dev, cap, 56); }
        if (type == 2 && byte(dev, cap + 2) >= 20) {
          v->notify = map_cap(dev, cap, size);
          v->notify_size = size;
          v->notify_stride = cfg(dev, cap + 16);
        }
        if (type == 3 && size) { v->isr = map_cap(dev, cap, 1); }
      }
      cap = byte(dev, cap + 1);
    }
    if (v->common && v->notify && v->isr) {
      v->type = (id >> 16) >= 0x1040 ? (id >> 16) - 0x1040 : cfg(dev, 0x2c) >> 16;
      cfg(dev, 4);
      uint16_t command = cfg(dev, 4) | 6;
      __asm__ volatile("outw %0,%w1" ::"a"(command), "d"(0xcfc));
    }
  }
}
uintptr_t virtio_slot(uint64_t hhdm, unsigned index, uint32_t type) {
  (void)hhdm;
  return index < 32 && slots[index].type == type ? (uintptr_t)&slots[index] : 0;
}
#define REG(width, off) (*(volatile uint##width##_t *)(v->common + (off)))
uint32_t virtio_read(uintptr_t base, unsigned reg) {
  struct pci_virtio *v = (void *)base;
  switch (reg) {
  case VIRTIO_MMIO_DEVICE_FEATURES:
    return REG(32, 4);
  case VIRTIO_MMIO_QUEUE_NUM_MAX:
    return REG(16, 24);
  case VIRTIO_MMIO_STATUS:
    return REG(8, 20);
  case VIRTIO_MMIO_INTERRUPT_STATUS:
    return *(volatile uint8_t *)v->isr;
  default:
    return 0;
  }
}
void virtio_write(uintptr_t base, unsigned reg, uint32_t value) {
  struct pci_virtio *v = (void *)base;
  switch (reg) {
  case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
    REG(32, 0) = value;
    break;
  case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
    REG(32, 8) = value;
    break;
  case VIRTIO_MMIO_DRIVER_FEATURES:
    REG(32, 12) = value;
    break;
  case VIRTIO_MMIO_STATUS:
    REG(8, 20) = value;
    break;
  case VIRTIO_MMIO_QUEUE_SEL:
    REG(16, 22) = value;
    break;
  case VIRTIO_MMIO_QUEUE_NUM:
    REG(16, 24) = value;
    break;
  case VIRTIO_MMIO_QUEUE_READY: {
    unsigned q = REG(16, 22);
    if (q < 2) { v->notify_offsets[q] = REG(16, 30); }
    REG(16, 28) = value;
    break;
  }
  case VIRTIO_MMIO_QUEUE_NOTIFY:
    if (value < 2) {
      uint64_t off = (uint64_t)v->notify_offsets[value] * v->notify_stride;
      if (off + 2 <= v->notify_size) { *(volatile uint16_t *)(v->notify + off) = value; }
    }
    break;
  case VIRTIO_MMIO_QUEUE_DESC_LOW:
  case VIRTIO_MMIO_QUEUE_DESC_LOW + 4:
    REG(32, 32 + reg - VIRTIO_MMIO_QUEUE_DESC_LOW) = value;
    break;
  case VIRTIO_MMIO_QUEUE_DRIVER_LOW:
  case VIRTIO_MMIO_QUEUE_DRIVER_LOW + 4:
    REG(32, 40 + reg - VIRTIO_MMIO_QUEUE_DRIVER_LOW) = value;
    break;
  case VIRTIO_MMIO_QUEUE_DEVICE_LOW:
  case VIRTIO_MMIO_QUEUE_DEVICE_LOW + 4:
    REG(32, 48 + reg - VIRTIO_MMIO_QUEUE_DEVICE_LOW) = value;
    break;
  }
  virtio_barrier();
}
