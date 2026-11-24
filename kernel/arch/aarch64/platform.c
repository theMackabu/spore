#include "arch/aarch64/platform.h"

#include "arch/smp.h"
#include "mm/pmm.h"

uint64_t aarch64_current_el(void) {
  uint64_t el;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
  return (el >> 2) & 0x3;
}

enum {
  PL011_PHYS = 0x09000000,
  VIRTIO_MMIO_PHYS = 0x0a000000,
  VIRTIO_MMIO_PAGES = 4,
  GICD_PHYS = 0x08000000,
  GICR_PHYS = 0x080a0000,
  GICR_SGI_PHYS = 0x080b0000,
  GICR_STRIDE = 0x20000,
};

void arch_halt(void) {
  for (;;) {
    __asm__ volatile("wfe");
  }
}

static void invalidate_boot_mapping(uint64_t *entry) {
  *entry = 0;
  __asm__ volatile("dsb ishst\ntlbi vmalle1is\ndsb ish\nisb" : : : "memory");
}

static void map_device_page(uint64_t hhdm_offset, uint64_t pa) {
  const uint64_t pa_mask = 0x0000fffffffff000ull;
  uint64_t va = hhdm_offset + pa;
  uint64_t ttbr1;
  __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));
  uint64_t *table = pmm_phys_to_virt(ttbr1 & pa_mask);

  for (unsigned shift = 39; shift > 12; shift -= 9) {
    uint64_t *entry = &table[(va >> shift) & 511];
    uint64_t desc = *entry;
    if ((desc & 3) != 3) {
      uint64_t next_pa = pmm_alloc_zero_page();
      if (next_pa == 0) { arch_halt(); }
      if (desc & 1) {
        uint64_t *next = pmm_phys_to_virt(next_pa);
        uint64_t stride = 1ull << (shift - 9);
        uint64_t base = (desc & pa_mask) & ~((1ull << shift) - 1);
        uint64_t attrs = desc & ~(pa_mask | 3ull | (1ull << 52));
        for (size_t i = 0; i < 512; ++i) {
          next[i] = (base + i * stride) | attrs | (shift == 21 ? 3ull : 1ull);
        }
        invalidate_boot_mapping(entry);
      }
      desc = next_pa | 3;
      *entry = desc;
      __asm__ volatile("dsb ishst" : : : "memory");
    }
    table = pmm_phys_to_virt(desc & pa_mask);
  }
  uint64_t *pte = &table[(va >> 12) & 511];
  if (*pte & 1) { invalidate_boot_mapping(pte); }
  *pte = pa | (2ull << 2) | (3ull << 8) | (1ull << 10) | (1ull << 53) | (1ull << 54) | 3;
}

void aarch64_map_devices(uint64_t hhdm_offset) {
  uint64_t mair;
  __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
  mair &= ~(0xffull << 16);
  __asm__ volatile("msr mair_el1, %0\nisb" : : "r"(mair) : "memory");
  map_device_page(hhdm_offset, PL011_PHYS);
  for (size_t i = 0; i < VIRTIO_MMIO_PAGES; ++i) {
    map_device_page(hhdm_offset, VIRTIO_MMIO_PHYS + i * PAGE_SIZE);
  }
  map_device_page(hhdm_offset, GICD_PHYS);
  for (uint32_t cpu = 0; cpu < smp_present_cpu_count(); ++cpu) {
    map_device_page(hhdm_offset, GICR_PHYS + cpu * GICR_STRIDE);
    map_device_page(hhdm_offset, GICR_SGI_PHYS + cpu * GICR_STRIDE);
  }
  __asm__ volatile("dsb ishst\ntlbi vmalle1is\ndsb ish\nisb" : : : "memory");
}

void arch_poweroff(void) {
  __asm__ volatile("mov x0, #0x0008\n"
                   "movk x0, #0x8400, lsl #16\n"
                   "hvc #0\n"
                   :
                   :
                   : "x0", "memory");
  arch_halt();
}

void arch_reboot(void) {
  __asm__ volatile("mov x0, #0x0009\n"
                   "movk x0, #0x8400, lsl #16\n"
                   "hvc #0\n"
                   :
                   :
                   : "x0", "memory");
  arch_halt();
}
