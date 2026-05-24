#include "mm/vmm.h"

#include "mem.h"
#include "mm/pmm.h"

enum {
  PTE_VALID = 1ull << 0,
  PTE_TABLE = 1ull << 1,
  PTE_AF = 1ull << 10,
  PTE_SH_INNER = 3ull << 8,
  PTE_AP_MASK = 3ull << 6,
  PTE_AP_USER_RW = 1ull << 6,
  PTE_AP_USER_RO = 3ull << 6,
};

#define PTE_PXN (1ull << 53)
#define PTE_UXN (1ull << 54)
#define PTE_COW (1ull << 55)
#define PTE_ADDR_MASK 0x0000fffffffff000ull

static uint64_t *pt_virt(const struct user_address_space *as, uint64_t pa) {
  return (uint64_t *)(uintptr_t)(as->hhdm_offset + pa);
}

static size_t pt_index(uint64_t va, unsigned shift) {
  return (size_t)((va >> shift) & 0x1ff);
}

static bool checked_add(uint64_t a, uint64_t b, uint64_t *out) {
  *out = a + b;
  return *out >= a;
}

static void flush_all_user_tlb(void) {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
  return;
#else
  __asm__ volatile("dsb ishst\n"
                   "tlbi vmalle1\n"
                   "dsb ish\n"
                   "isb\n"
                   :
                   :
                   : "memory");
#endif
}

bool vmm_user_init(struct user_address_space *as, uint64_t hhdm_offset) {
  uint64_t root = pmm_alloc_zero_page();
  if (root == 0) { return false; }
  as->root_pa = root;
  as->hhdm_offset = hhdm_offset;
  as->brk_base = 0;
  as->brk_current = 0;
  as->mmap_base = 0x0000007000000000ull;
  as->asid = 0;
  return true;
}

static bool ensure_table(struct user_address_space *as, uint64_t *table, size_t index, uint64_t **next) {
  uint64_t entry = table[index];
  if ((entry & PTE_VALID) == 0) {
    uint64_t pa = pmm_alloc_zero_page();
    if (pa == 0) { return false; }
    table[index] = pa | PTE_VALID | PTE_TABLE;
    entry = table[index];
  }
  if ((entry & PTE_TABLE) == 0) { return false; }
  *next = pt_virt(as, entry & PTE_ADDR_MASK);
  return true;
}

bool vmm_map_page(struct user_address_space *as, uint64_t va, uint64_t pa, uint32_t flags) {
  uint64_t *l0 = pt_virt(as, as->root_pa);
  uint64_t *l1;
  uint64_t *l2;
  uint64_t *l3;

  if (!ensure_table(as, l0, pt_index(va, 39), &l1) || !ensure_table(as, l1, pt_index(va, 30), &l2) ||
      !ensure_table(as, l2, pt_index(va, 21), &l3)) {
    return false;
  }

  uint64_t ap = (flags & VMM_USER_WRITE) != 0 ? PTE_AP_USER_RW : PTE_AP_USER_RO;
  uint64_t xn = (flags & VMM_USER_EXEC) != 0 ? 0 : PTE_UXN;
  l3[pt_index(va, 12)] = (pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER | ap | PTE_PXN | xn;
  return true;
}

bool vmm_alloc_page(struct user_address_space *as, uint64_t va, uint32_t flags) {
  uint64_t pa = pmm_alloc_zero_page();
  return pa != 0 && vmm_map_page(as, va, pa, flags);
}

static bool is_leaf_at_level(uint64_t entry, unsigned level) {
  if ((entry & PTE_VALID) == 0) { return false; }
  return level == 3 || (entry & PTE_TABLE) == 0;
}

static bool is_writable_leaf(uint64_t entry) {
  return (entry & PTE_AP_MASK) == PTE_AP_USER_RW;
}

static uint64_t leaf_with_ap(uint64_t entry, uint64_t ap) {
  return (entry & ~PTE_AP_MASK) | ap;
}

static bool clone_table_cow(struct user_address_space *dst, struct user_address_space *src, uint64_t dst_table_pa,
                            uint64_t src_table_pa, unsigned level) {
  uint64_t *dst_table = pt_virt(dst, dst_table_pa);
  uint64_t *src_table = pt_virt(src, src_table_pa);

  for (size_t i = 0; i < 512; ++i) {
    uint64_t entry = src_table[i];
    if ((entry & PTE_VALID) == 0) { continue; }

    if (is_leaf_at_level(entry, level)) {
      uint64_t pa = entry & PTE_ADDR_MASK;
      if (!pmm_share_page(pa)) { return false; }
      if (is_writable_leaf(entry) || (entry & PTE_COW) != 0) {
        entry = leaf_with_ap(entry | PTE_COW, PTE_AP_USER_RO);
        src_table[i] = entry;
      }
      dst_table[i] = entry;
      continue;
    }

    uint64_t child_pa = pmm_alloc_zero_page();
    if (child_pa == 0) { return false; }
    dst_table[i] = child_pa | PTE_VALID | PTE_TABLE;
    if (!clone_table_cow(dst, src, child_pa, entry & PTE_ADDR_MASK, level + 1)) { return false; }
  }
  return true;
}

bool vmm_clone_cow(struct user_address_space *dst, struct user_address_space *src, uint16_t asid) {
  uint64_t root = pmm_alloc_zero_page();
  if (root == 0) { return false; }
  dst->root_pa = root;
  dst->hhdm_offset = src->hhdm_offset;
  dst->brk_base = src->brk_base;
  dst->brk_current = src->brk_current;
  dst->mmap_base = src->mmap_base;
  dst->asid = asid;

  // v1 is cooperative and uniprocessor, so source PTE downgrades need no lock.
  // Hardware DBM is left off; CoW is represented only by AP[2] + PTE_COW.
  bool ok = clone_table_cow(dst, src, root, src->root_pa, 0);
  flush_all_user_tlb();
  return ok;
}

static void destroy_table(struct user_address_space *as, uint64_t table_pa, unsigned level) {
  uint64_t *table = pt_virt(as, table_pa);
  for (size_t i = 0; i < 512; ++i) {
    uint64_t entry = table[i];
    if ((entry & PTE_VALID) == 0) { continue; }
    uint64_t pa = entry & PTE_ADDR_MASK;
    if (is_leaf_at_level(entry, level)) {
      pmm_free_page(pa);
    } else {
      destroy_table(as, pa, level + 1);
    }
  }
  pmm_free_page(table_pa);
}

void vmm_destroy(struct user_address_space *as) {
  if (as->root_pa != 0) {
    destroy_table(as, as->root_pa, 0);
    as->root_pa = 0;
  }
}

static bool user_entry_allows(uint64_t entry, enum vmm_access access) {
  switch (access) {
  case VMM_ACCESS_READ:
    return true;
  case VMM_ACCESS_WRITE:
    return (entry & PTE_AP_MASK) == PTE_AP_USER_RW;
  case VMM_ACCESS_EXEC:
    return (entry & PTE_UXN) == 0;
  }
  return false;
}

static uint64_t user_to_phys_checked(const struct user_address_space *as, uint64_t va, enum vmm_access access) {
  const uint64_t *l0 = pt_virt(as, as->root_pa);
  uint64_t entry = l0[pt_index(va, 39)];
  if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) { return 0; }
  const uint64_t *l1 = pt_virt(as, entry & PTE_ADDR_MASK);
  entry = l1[pt_index(va, 30)];
  if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) { return 0; }
  const uint64_t *l2 = pt_virt(as, entry & PTE_ADDR_MASK);
  entry = l2[pt_index(va, 21)];
  if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) { return 0; }
  const uint64_t *l3 = pt_virt(as, entry & PTE_ADDR_MASK);
  entry = l3[pt_index(va, 12)];
  if ((entry & PTE_VALID) == 0) { return 0; }
  if (!user_entry_allows(entry, access)) { return 0; }
  return (entry & PTE_ADDR_MASK) | (va & 0xfff);
}

static uint64_t *leaf_for_va(const struct user_address_space *as, uint64_t va) {
  uint64_t *l0 = pt_virt(as, as->root_pa);
  uint64_t entry = l0[pt_index(va, 39)];
  if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) { return NULL; }
  uint64_t *l1 = pt_virt(as, entry & PTE_ADDR_MASK);
  entry = l1[pt_index(va, 30)];
  if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) { return NULL; }
  uint64_t *l2 = pt_virt(as, entry & PTE_ADDR_MASK);
  entry = l2[pt_index(va, 21)];
  if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) { return NULL; }
  uint64_t *l3 = pt_virt(as, entry & PTE_ADDR_MASK);
  return &l3[pt_index(va, 12)];
}

bool vmm_handle_cow_fault(struct user_address_space *as, uint64_t va) {
  uint64_t *pte = leaf_for_va(as, va);
  if (pte == NULL || (*pte & (PTE_VALID | PTE_COW)) != (PTE_VALID | PTE_COW)) { return false; }

  uint64_t old_entry = *pte;
  uint64_t old_pa = old_entry & PTE_ADDR_MASK;
  if (pmm_is_last_ref(old_pa)) {
    *pte = leaf_with_ap(old_entry & ~PTE_COW, PTE_AP_USER_RW);
    vmm_flush_user_va(va);
    return true;
  }

  uint64_t new_pa = pmm_alloc_zero_page();
  if (new_pa == 0) { return false; }
  kmemcpy((void *)(uintptr_t)(as->hhdm_offset + new_pa), (const void *)(uintptr_t)(as->hhdm_offset + old_pa),
          PAGE_SIZE);

  *pte = 0;
  vmm_flush_user_va(va);
  uint64_t new_entry = leaf_with_ap((old_entry & ~PTE_ADDR_MASK & ~PTE_COW) | (new_pa & PTE_ADDR_MASK), PTE_AP_USER_RW);
  *pte = new_entry;
  pmm_free_page(old_pa);
  vmm_flush_user_va(va);
  return true;
}

uint64_t vmm_user_to_phys(const struct user_address_space *as, uint64_t va) {
  return user_to_phys_checked(as, va, VMM_ACCESS_READ);
}

bool vmm_is_mapped(const struct user_address_space *as, uint64_t va) {
  uint64_t *pte = leaf_for_va(as, va);
  return pte != NULL && (*pte & PTE_VALID) != 0;
}

void vmm_unmap_range(struct user_address_space *as, uint64_t start, uint64_t end) {
  for (uint64_t va = start; va < end; va += PAGE_SIZE) {
    uint64_t *pte = leaf_for_va(as, va);
    if (pte == NULL || (*pte & PTE_VALID) == 0) { continue; }
    uint64_t pa = *pte & PTE_ADDR_MASK;
    *pte = 0;
    pmm_free_page(pa);
    vmm_flush_user_va(va);
  }
}

void vmm_protect_range(struct user_address_space *as, uint64_t start, uint64_t end, uint32_t flags) {
  for (uint64_t va = start; va < end; va += PAGE_SIZE) {
    uint64_t *pte = leaf_for_va(as, va);
    if (pte == NULL || (*pte & PTE_VALID) == 0) { continue; }
    uint64_t entry = *pte;
    uint64_t ap = (flags & VMM_USER_WRITE) != 0 ? PTE_AP_USER_RW : PTE_AP_USER_RO;
    entry = leaf_with_ap(entry, ap);
    if ((flags & VMM_USER_EXEC) != 0) {
      entry &= ~PTE_UXN;
    } else {
      entry |= PTE_UXN;
    }
    *pte = entry;
    vmm_flush_user_va(va);
  }
}

size_t vmm_mapped_pages_in_range(const struct user_address_space *as, uint64_t start, uint64_t end) {
  size_t pages = 0;
  for (uint64_t va = start; va < end; va += PAGE_SIZE) {
    if (vmm_is_mapped(as, va)) { ++pages; }
  }
  return pages;
}

bool vmm_user_range_accessible(const struct user_address_space *as, uint64_t va, size_t len, enum vmm_access access) {
  if (len == 0) { return true; }
  uint64_t end;
  if (!checked_add(va, len - 1, &end)) { return false; }

  uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
  uint64_t last = end & ~(uint64_t)(PAGE_SIZE - 1);
  for (;;) {
    if (user_to_phys_checked(as, page, access) == 0) { return false; }
    if (page == last) { return true; }
    page += PAGE_SIZE;
  }
}

bool vmm_copy_to_user(const struct user_address_space *as, uint64_t dst, const void *src, size_t len) {
  struct user_address_space *mutable_as = (struct user_address_space *)as;
  for (size_t i = 0; i < len; ++i) {
    if (user_to_phys_checked(as, dst + i, VMM_ACCESS_WRITE) == 0 && !vmm_handle_cow_fault(mutable_as, dst + i)) {
      return false;
    }
  }
  if (!vmm_user_range_accessible(as, dst, len, VMM_ACCESS_WRITE)) { return false; }
  const uint8_t *s = src;
  for (size_t i = 0; i < len; ++i) {
    uint64_t pa = user_to_phys_checked(as, dst + i, VMM_ACCESS_WRITE);
    *(uint8_t *)(uintptr_t)(as->hhdm_offset + pa) = s[i];
  }
  return true;
}

bool vmm_copy_from_user(const struct user_address_space *as, void *dst, uint64_t src, size_t len) {
  if (!vmm_user_range_accessible(as, src, len, VMM_ACCESS_READ)) { return false; }
  uint8_t *d = dst;
  for (size_t i = 0; i < len; ++i) {
    uint64_t pa = user_to_phys_checked(as, src + i, VMM_ACCESS_READ);
    d[i] = *(uint8_t *)(uintptr_t)(as->hhdm_offset + pa);
  }
  return true;
}

void vmm_enable_ttbr0(void) {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
  return;
#else
  uint64_t tcr;
  __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
  const uint64_t clear = 0x3full | (1ull << 7) | (3ull << 8) | (3ull << 10) | (3ull << 12) | (3ull << 14);
  tcr &= ~clear;
  tcr |= 16ull | (1ull << 8) | (1ull << 10) | (3ull << 12);
  __asm__ volatile("msr tcr_el1, %0\n"
                   "isb\n"
                   :
                   : "r"(tcr)
                   : "memory");
#endif
}

void vmm_install_user(const struct user_address_space *as) {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
  (void)as;
  return;
#else
  uint64_t ttbr = as->root_pa | ((uint64_t)as->asid << 48);
  __asm__ volatile("msr ttbr0_el1, %0\n"
                   "dsb ishst\n"
                   "tlbi vmalle1is\n"
                   "dsb ish\n"
                   "isb\n"
                   :
                   : "r"(ttbr)
                   : "memory");
#endif
}

void vmm_flush_user_va(uint64_t va) {
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
  (void)va;
  return;
#else
  uint64_t op = va >> 12;
  __asm__ volatile("dsb ishst\n"
                   "tlbi vae1is, %0\n"
                   "dsb ish\n"
                   "isb\n"
                   :
                   : "r"(op)
                   : "memory");
#endif
}
