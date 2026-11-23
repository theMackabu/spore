#pragma once

#include <stdint.h>

uint64_t aarch64_current_el(void);
[[noreturn]] void aarch64_halt(void);
void aarch64_map_devices(uint64_t hhdm_offset);
[[noreturn]] void aarch64_poweroff(void);
[[noreturn]] void aarch64_reboot(void);

static inline void aarch64_io_barrier(void) {
  __asm__ volatile("dsb sy" : : : "memory");
}

static inline void aarch64_cpu_relax(void) {
  __asm__ volatile("yield");
}
