#pragma once

#include "user.h"
#include <stdint.h>

static inline void native_publish_table(uint64_t *entry, uint64_t descriptor) {
  __atomic_store_n(entry, descriptor, __ATOMIC_RELEASE);
}
static inline void invalidate_local(uint64_t va) {
#ifdef __riscv
  __asm__ volatile("sfence.vma %0" :: "r"(va) : "memory");
#else
  __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
#endif
}
static inline void native_flush_va(uint64_t va) {
  invalidate_local(va);
  port_flush_remote();
}
static inline void native_flush_all(void) {
#ifdef __riscv
  __asm__ volatile("sfence.vma" ::: "memory");
#else
  install_root(active_root());
#endif
  port_flush_remote();
}
static inline void native_flush_pages(uint64_t va, uint64_t mask) {
  if (!mask) { return; }
  while (mask) {
    invalidate_local(va + (uint64_t)__builtin_ctzll(mask) * PAGE_SIZE);
    mask &= mask - 1;
  }
  port_flush_remote();
}
