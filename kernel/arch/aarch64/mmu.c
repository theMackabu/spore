#include "arch/aarch64/mmu.h"

#include "arch/smp.h"
#include "mm/vmm.h"

void aarch64_publish_table(uint64_t *slot, uint64_t descriptor) {
#if !__STDC_HOSTED__
  __asm__ volatile("dsb ishst" ::: "memory");
#endif
  *slot = descriptor;
}

void aarch64_tlb_flush_all(void) {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
  return;
#else
  __asm__ volatile("dsb ishst\n"
                   "tlbi vmalle1is\n"
                   "dsb ish\n"
                   "isb\n"
                   :
                   :
                   : "memory");
#endif
}

void aarch64_enable_ttbr0(void) {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
  return;
#else
  uint64_t tcr;
  __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
  const uint64_t clear =
    0x3full | (1ull << 7) | (3ull << 8) | (3ull << 10) | (3ull << 12) | (3ull << 14) | (1ull << 22);
  tcr = (tcr & ~clear) | 16ull | (1ull << 8) | (1ull << 10) | (3ull << 12);
  __asm__ volatile("msr tcr_el1, %0\n"
                   "isb\n"
                   :
                   : "r"(tcr)
                   : "memory");
  aarch64_tlb_flush_all();
#endif
}

void vmm_install_user(const struct user_address_space *as) {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
  (void)as;
  return;
#else
  static uint64_t installed_context[SPORE_BOOT_CPU_MAX];
  uint32_t cpu = smp_current_cpu();
  if (installed_context[cpu] == as->context_id && as->context_id != 0) { return; }
  uint64_t tag = ((uint64_t)cpu + 1) << 48;
  uint64_t ttbr = as->root_pa | tag;
  __asm__ volatile("dsb ishst\n"
                   "msr ttbr0_el1, %0\n"
                   "isb\n"
                   "tlbi aside1is, %1\n"
                   "dsb ish\n"
                   "isb\n"
                   :
                   : "r"(ttbr), "r"(tag)
                   : "memory");
  installed_context[cpu] = as->context_id;
#endif
}

void aarch64_tlb_flush_pages(uint64_t first_va, uint64_t mask) {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
  (void)first_va;
  (void)mask;
#else
  if (mask == 0) { return; }
  __asm__ volatile("dsb ishst" ::: "memory");
  uint64_t first_page = first_va >> 12;
  do {
    uint64_t page = first_page + (unsigned)__builtin_ctzll(mask);
    __asm__ volatile("tlbi vaae1is, %0" :: "r"(page) : "memory");
    mask &= mask - 1;
  } while (mask);
  __asm__ volatile("dsb ish\nisb" ::: "memory");
#endif
}

void aarch64_tlb_flush_va(uint64_t va) {
  aarch64_tlb_flush_pages(va, 1);
}
