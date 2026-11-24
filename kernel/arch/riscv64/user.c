#include "user.h"
#include "drivers/virtio_console.h"

uint64_t active_root(void) {
  uint64_t satp;
  __asm__ volatile("csrr %0,satp" : "=r"(satp));
  return (satp & ((1ull << 44) - 1)) << 12;
}
void install_root(uint64_t pa) {
  __asm__ volatile("csrw satp,%0; sfence.vma" ::"r"((9ull << 60) | (pa >> 12)) : "memory");
}
unsigned page_levels(void) {
  return 4;
}
uint64_t entry_phys(uint64_t pte) {
  return (pte >> 10) << 12;
}
uint64_t table_entry(uint64_t pa) {
  return ((pa >> 12) << 10) | 1;
}
uint64_t page_entry(uint64_t pa, unsigned flags) {
  return table_entry(pa) | 0xc0 | (flags << 1);
}

bool status_init(uint64_t hhdm) {
  constexpr uint64_t base = 0xffffffef00000000ull;
  for (unsigned i = 0; i < 8; ++i) {
    if (!map_page(base + PAGE_SIZE * i, 0x10001000 + PAGE_SIZE * i, PAGE_READ | PAGE_WRITE)) { return false; }
  }
  __asm__ volatile("sfence.vma" ::: "memory");
  return virtio_console_init_at(hhdm, base, 8, PAGE_SIZE);
}
void status_write(const uint8_t *bytes, size_t n) {
  virtio_console_write(bytes, n);
}

extern void trap_entry(void);
void platform_cpu_init(uint32_t cpu, uintptr_t stack) {
  (void)cpu;
  (void)stack;
  extern void platform_rearm_timer(void);
  __asm__ volatile("csrw stvec,%0; csrc sstatus,%1; csrs sstatus,%2; csrs sie,%3" ::"r"((uintptr_t)trap_entry),
                   "r"(0x40102ul), "r"(0x2020ul), "r"(32ul)
                   : "memory");
  platform_rearm_timer();
}
