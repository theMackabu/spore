#pragma once

#include <stdbool.h>
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
  VIRTIO_MMIO_QUEUE_DRIVER_LOW = 0x090,
  VIRTIO_MMIO_QUEUE_DEVICE_LOW = 0x0a0,
  VIRTIO_MAGIC = 0x74726976,
  VIRTIO_STATUS_ACKNOWLEDGE = 1,
  VIRTIO_STATUS_DRIVER = 2,
  VIRTIO_STATUS_DRIVER_OK = 4,
  VIRTIO_STATUS_FEATURES_OK = 8,
  VIRTIO_F_VERSION_1_BIT = 0,
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
  uint16_t ring[];
};

struct virtq_used_elem {
  uint32_t id;
  uint32_t len;
};

struct virtq_used {
  uint16_t flags;
  uint16_t idx;
  struct virtq_used_elem ring[];
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

#if defined(__riscv)
static inline void virtio_barrier(void) {
  __asm__ volatile("fence iorw, iorw" ::: "memory");
}
#elif defined(__x86_64__)
static inline void virtio_barrier(void) {
  __asm__ volatile("mfence" ::: "memory");
}
#else
#include "arch/aarch64/platform.h"
#define virtio_barrier aarch64_io_barrier
#endif

uintptr_t virtio_slot(uint64_t hhdm, unsigned index, uint32_t device);
#if defined(SPORE_NATIVE_PORT) && defined(__x86_64__)
uint32_t virtio_read(uintptr_t base, unsigned reg);
void virtio_write(uintptr_t base, unsigned reg, uint32_t value);
#else
static inline uint32_t virtio_read(uintptr_t base, unsigned offset) {
  return *(volatile uint32_t *)(base + offset);
}

static inline void virtio_write(uintptr_t base, unsigned offset, uint32_t value) {
  *(volatile uint32_t *)(base + offset) = value;
  virtio_barrier();
}

#endif

static inline void virtio_set_status(uintptr_t base, uint32_t bits) {
  virtio_write(base, VIRTIO_MMIO_STATUS, virtio_read(base, VIRTIO_MMIO_STATUS) | bits);
}

static inline void virtq_publish(struct virtq *q, unsigned size, uint16_t id) {
  q->avail->ring[q->avail_idx % size] = id;
  virtio_barrier();
  q->avail->idx = ++q->avail_idx;
  virtio_barrier();
}

bool virtio_matches(uintptr_t base, uint32_t device);
uintptr_t virtio_find(uintptr_t base, unsigned slots, unsigned stride, uint32_t device);
enum virtio_features { VIRTIO_FEATURES_OK, VIRTIO_VERSION_MISSING, VIRTIO_FEATURES_REJECTED };
enum virtio_features virtio_negotiate(uintptr_t base);
uint64_t virtio_alloc_page(uint64_t hhdm, void **virt);
bool virtq_alloc(struct virtq *q, uint64_t hhdm);
bool virtq_setup(uintptr_t base, unsigned index, unsigned size, struct virtq *q);
