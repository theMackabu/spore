#include "mm/vmm.h"

#include "arch/aarch64/mmu.h"
#include "lib/mem.h"
#include "mm/pmm.h"

enum {
  PTE_VALID = 1ull << 0,
  PTE_TABLE = 1ull << 1,
  PTE_NG = 1ull << 11,
  PTE_AF = 1ull << 10,
  PTE_SH_INNER = 3ull << 8,
  PTE_AP_MASK = 3ull << 6,
  PTE_AP_USER_RW = 1ull << 6,
  PTE_AP_USER_RO = 3ull << 6,
};

constexpr uint64_t PTE_PXN = 1ull << 53;
constexpr uint64_t PTE_UXN = 1ull << 54;
constexpr uint64_t PTE_COW = 1ull << 55;
constexpr uint64_t PTE_SHARED = 1ull << 56;
constexpr uint64_t PTE_WRITE_ALLOWED = 1ull << 57;
constexpr uint64_t PTE_ADDR_MASK = 0x0000fffffffff000ull;

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

static uint64_t *walk_to_leaf(const struct user_address_space *as, uint64_t va, unsigned *span_shift) {
  uint64_t *table = pt_virt(as, as->root_pa);
  for (unsigned shift = 39; shift > 12; shift -= 9) {
    uint64_t entry = table[pt_index(va, shift)];
    if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) {
      *span_shift = shift;
      return NULL;
    }
    table = pt_virt(as, entry & PTE_ADDR_MASK);
  }
  *span_shift = 21;
  return &table[pt_index(va, 12)];
}

static uint64_t *leaf_for_va(const struct user_address_space *as, uint64_t va) {
  unsigned shift;
  return walk_to_leaf(as, va, &shift);
}

struct leaf_range {
  uint64_t next_page;
  uint64_t limit;
  uint64_t va;
  uint64_t *ptes;
  size_t count;
};

static struct leaf_range leaf_range(uint64_t start, uint64_t end) {
  uint64_t first = start / PAGE_SIZE;
  uint64_t pages = end > start ? 1 + (end - 1 - start) / PAGE_SIZE : 0;
  return (struct leaf_range){.next_page = first, .limit = first + pages};
}

static bool next_leaf_range(const struct user_address_space *as, struct leaf_range *range) {
  while (range->next_page < range->limit) {
    uint64_t page = range->next_page;
    range->va = page * PAGE_SIZE;
    unsigned shift;
    range->ptes = walk_to_leaf(as, range->va, &shift);
    uint64_t span = 1ull << (shift - 12);
    uint64_t next = (page & ~(span - 1)) + span;
    range->next_page = next < range->limit ? next : range->limit;
    if (range->ptes != NULL) {
      range->count = range->next_page - page;
      return true;
    }
  }
  return false;
}

bool vmm_user_init(struct user_address_space *as, uint64_t hhdm_offset) {
  static uint64_t last_context_id;
  uint64_t root = pmm_alloc_zero_page();
  if (root == 0) { return false; }
  as->context_id = __atomic_add_fetch(&last_context_id, 1, __ATOMIC_RELAXED);
  as->root_pa = root;
  as->hhdm_offset = hhdm_offset;
  as->resident_pages = 0;
  as->peak_resident_pages = 0;
  as->arg_start = 0;
  as->arg_end = 0;
  as->brk_base = 0;
  as->brk_current = 0;
  as->mmap_base = USER_MMAP_BASE;
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

static uint64_t with_protection(uint64_t entry, uint32_t flags) {
  entry &= ~(PTE_AP_MASK | PTE_UXN | PTE_WRITE_ALLOWED);
  if (!(flags & VMM_USER_EXEC)) { entry |= PTE_UXN; }
  if (flags & VMM_USER_WRITE) {
    entry |= PTE_WRITE_ALLOWED | ((entry & PTE_COW) ? PTE_AP_USER_RO : PTE_AP_USER_RW);
  } else if (flags & (VMM_USER_READ | VMM_USER_EXEC)) {
    entry |= PTE_AP_USER_RO;
  }
  return entry;
}

static bool map_user_page(struct user_address_space *as, uint64_t va, uint64_t pa, uint32_t flags,
                          uint64_t permissions) {
  uint64_t *l0 = pt_virt(as, as->root_pa);
  uint64_t *l1;
  uint64_t *l2;
  uint64_t *l3;

  if (!ensure_table(as, l0, pt_index(va, 39), &l1) || !ensure_table(as, l1, pt_index(va, 30), &l2) ||
      !ensure_table(as, l2, pt_index(va, 21), &l3)) {
    return false;
  }

  uint64_t entry = (pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE | PTE_NG | PTE_AF | PTE_SH_INNER | PTE_PXN | permissions;
  if (flags & VMM_MAP_SHARED) { entry |= PTE_SHARED; }
  uint64_t *pte = &l3[pt_index(va, 12)];
  if (*pte & PTE_VALID) { return false; }
  *pte = with_protection(entry, flags);
  ++as->resident_pages;
  if (as->resident_pages > as->peak_resident_pages) { as->peak_resident_pages = as->resident_pages; }
  aarch64_tlb_flush_va(va);
  return true;
}

bool vmm_map_page(struct user_address_space *as, uint64_t va, uint64_t pa, uint32_t flags) {
  return map_user_page(as, va, pa, flags, 0);
}

bool vmm_map_page_cow(struct user_address_space *as, uint64_t va, uint64_t pa, uint32_t flags) {
  return map_user_page(as, va, pa, flags, PTE_COW);
}

bool vmm_alloc_page(struct user_address_space *as, uint64_t va, uint32_t flags) {
  uint64_t pa = pmm_alloc_zero_page();
  if (pa == 0) { return false; }
  if (vmm_map_page(as, va, pa, flags)) { return true; }
  pmm_free_page(pa);
  return false;
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

static bool clone_table(struct user_address_space *dst, struct user_address_space *src, uint64_t dst_table_pa,
                            uint64_t src_table_pa, unsigned level, enum vmm_clone_mode mode) {
  uint64_t *dst_table = pt_virt(dst, dst_table_pa);
  uint64_t *src_table = pt_virt(src, src_table_pa);

  for (size_t i = 0; i < 512; ++i) {
    uint64_t entry = src_table[i];
    if ((entry & PTE_VALID) == 0) { continue; }

    if (is_leaf_at_level(entry, level)) {
      uint64_t pa = entry & PTE_ADDR_MASK;
      if ((entry & PTE_SHARED) && mode == VMM_SNAPSHOT) {
        uint64_t copy = pmm_alloc_page();
        if (copy == 0) { return false; }
        kmemcpy(pt_virt(dst, copy), pt_virt(src, pa), PAGE_SIZE);
        entry = (entry & ~PTE_ADDR_MASK) | copy;
      } else {
        if (!pmm_share_page(pa)) { return false; }
        if (!(entry & PTE_SHARED)) {
          entry |= PTE_COW;
          if (is_writable_leaf(entry)) { entry = leaf_with_ap(entry, PTE_AP_USER_RO); }
          src_table[i] = entry;
        }
      }
      dst_table[i] = entry;
      continue;
    }

    uint64_t child_pa = pmm_alloc_zero_page();
    if (child_pa == 0) { return false; }
    dst_table[i] = child_pa | PTE_VALID | PTE_TABLE;
    if (!clone_table(dst, src, child_pa, entry & PTE_ADDR_MASK, level + 1, mode)) { return false; }
  }
  return true;
}

bool vmm_clone(struct user_address_space *dst, struct user_address_space *src, enum vmm_clone_mode mode) {
  if (!vmm_user_init(dst, src->hhdm_offset)) { return false; }
  dst->arg_start = src->arg_start;
  dst->arg_end = src->arg_end;
  dst->brk_base = src->brk_base;
  dst->brk_current = src->brk_current;
  dst->mmap_base = src->mmap_base;

  bool ok = clone_table(dst, src, dst->root_pa, src->root_pa, 0, mode);
  if (ok) {
    dst->resident_pages = src->resident_pages;
    dst->peak_resident_pages = src->resident_pages;
  } else {
    vmm_destroy(dst);
  }
  aarch64_tlb_flush_all();
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
    as->resident_pages = 0;
  }
}

static bool user_entry_allows(uint64_t entry, enum vmm_access access) {
  switch (access) {
  case VMM_ACCESS_READ:
    return (entry & PTE_AP_USER_RW) != 0;
  case VMM_ACCESS_WRITE:
    return (entry & PTE_AP_MASK) == PTE_AP_USER_RW;
  case VMM_ACCESS_EXEC:
    return (entry & PTE_UXN) == 0;
  }
  return false;
}

static uint64_t user_to_phys_checked(const struct user_address_space *as, uint64_t va, enum vmm_access access) {
  const uint64_t *pte = leaf_for_va(as, va);
  if (pte == NULL) { return 0; }
  uint64_t entry = *pte;
  if ((entry & PTE_VALID) == 0) { return 0; }
  if (!user_entry_allows(entry, access)) { return 0; }
  return (entry & PTE_ADDR_MASK) | (va & 0xfff);
}

bool vmm_handle_cow_fault(struct user_address_space *as, uint64_t va) {
  uint64_t *pte = leaf_for_va(as, va);
  constexpr uint64_t writable_cow = PTE_VALID | PTE_COW | PTE_WRITE_ALLOWED;
  if (pte == NULL || (*pte & writable_cow) != writable_cow) { return false; }

  uint64_t old_entry = *pte;
  uint64_t old_pa = old_entry & PTE_ADDR_MASK;
  if (pmm_is_last_ref(old_pa)) {
    *pte = leaf_with_ap(old_entry & ~PTE_COW, PTE_AP_USER_RW);
    aarch64_tlb_flush_va(va);
    return true;
  }

  uint64_t new_pa = pmm_alloc_page();
  if (new_pa == 0) { return false; }
  kmemcpy((void *)(uintptr_t)(as->hhdm_offset + new_pa), (const void *)(uintptr_t)(as->hhdm_offset + old_pa),
          PAGE_SIZE);

  *pte = 0;
  aarch64_tlb_flush_va(va);
  uint64_t new_entry = leaf_with_ap((old_entry & ~PTE_ADDR_MASK & ~PTE_COW) | (new_pa & PTE_ADDR_MASK), PTE_AP_USER_RW);
  *pte = new_entry;
  pmm_free_page(old_pa);
  aarch64_tlb_flush_va(va);
  return true;
}

uint64_t vmm_user_to_phys(const struct user_address_space *as, uint64_t va) {
  return user_to_phys_checked(as, va, VMM_ACCESS_READ);
}

bool vmm_is_mapped(const struct user_address_space *as, uint64_t va) {
  uint64_t *pte = leaf_for_va(as, va);
  return pte != NULL && (*pte & PTE_VALID) != 0;
}

bool vmm_move_page(struct user_address_space *as, uint64_t old_va, uint64_t new_va) {
  uint64_t *old_pte = leaf_for_va(as, old_va);
  if (old_pte == NULL || (*old_pte & PTE_VALID) == 0) { return true; }

  uint64_t *l0 = pt_virt(as, as->root_pa);
  uint64_t *l1;
  uint64_t *l2;
  uint64_t *l3;
  if (!ensure_table(as, l0, pt_index(new_va, 39), &l1) || !ensure_table(as, l1, pt_index(new_va, 30), &l2) ||
      !ensure_table(as, l2, pt_index(new_va, 21), &l3)) {
    return false;
  }

  uint64_t *new_pte = &l3[pt_index(new_va, 12)];
  if ((*new_pte & PTE_VALID) != 0) { return false; }
  *new_pte = *old_pte;
  *old_pte = 0;
  aarch64_tlb_flush_va(old_va);
  aarch64_tlb_flush_va(new_va);
  return true;
}

void vmm_unmap_range(struct user_address_space *as, uint64_t start, uint64_t end) {
  struct leaf_range range = leaf_range(start, end);
  while (next_leaf_range(as, &range)) {
    for (size_t i = 0; i < range.count; ++i) {
      uint64_t entry = range.ptes[i];
      if ((entry & PTE_VALID) == 0) { continue; }
      range.ptes[i] = 0;
      --as->resident_pages;
      aarch64_tlb_flush_va(range.va + i * PAGE_SIZE);
      pmm_free_page(entry & PTE_ADDR_MASK);
    }
  }
}

void vmm_protect_range(struct user_address_space *as, uint64_t start, uint64_t end, uint32_t flags) {
  struct leaf_range range = leaf_range(start, end);
  while (next_leaf_range(as, &range)) {
    for (size_t i = 0; i < range.count; ++i) {
      uint64_t entry = range.ptes[i];
      if ((entry & PTE_VALID) == 0) { continue; }
      range.ptes[i] = with_protection(entry, flags);
      aarch64_tlb_flush_va(range.va + i * PAGE_SIZE);
    }
  }
}

size_t vmm_mapped_pages_in_range(const struct user_address_space *as, uint64_t start, uint64_t end) {
  size_t pages = 0;
  struct leaf_range range = leaf_range(start, end);
  while (next_leaf_range(as, &range)) {
    for (size_t i = 0; i < range.count; ++i) {
      pages += (range.ptes[i] & PTE_VALID) != 0;
    }
  }
  return pages;
}

bool vmm_user_range_accessible(const struct user_address_space *as, uint64_t va, size_t len, enum vmm_access access) {
  if (len == 0) { return true; }
  uint64_t end;
  if (!checked_add(va, len - 1, &end)) { return false; }

  uint64_t page = va / PAGE_SIZE;
  uint64_t limit = end / PAGE_SIZE + 1;
  while (page < limit) {
    const uint64_t *ptes = leaf_for_va(as, page * PAGE_SIZE);
    if (ptes == NULL) { return false; }
    size_t count = 512 - page % 512;
    if (count > limit - page) { count = limit - page; }
    for (size_t i = 0; i < count; ++i) {
      uint64_t entry = ptes[i];
      if (!(entry & PTE_VALID) || !(entry & PTE_ADDR_MASK) || !user_entry_allows(entry, access)) { return false; }
    }
    page += count;
  }
  return true;
}

bool vmm_copy_to_user(const struct user_address_space *as, uint64_t dst, const void *src, size_t len) {
  if (len == 0) { return true; }
  uint64_t end;
  if (!checked_add(dst, len - 1, &end)) { return false; }
  struct user_address_space *mutable_as = (struct user_address_space *)as;
  if (dst / PAGE_SIZE != end / PAGE_SIZE) {
    uint64_t first = dst / PAGE_SIZE;
    uint64_t last = end / PAGE_SIZE;
    for (uint64_t page = first; page <= last; ++page) {
      uint64_t va = page * PAGE_SIZE;
      if (user_to_phys_checked(as, va, VMM_ACCESS_WRITE) == 0 && !vmm_handle_cow_fault(mutable_as, va)) {
        return false;
      }
    }
  }

  const uint8_t *s = src;
  size_t done = 0;
  while (done < len) {
    uint64_t va = dst + done;
    uint64_t page_off = va & (PAGE_SIZE - 1);
    size_t chunk = PAGE_SIZE - page_off;
    if (chunk > len - done) { chunk = len - done; }

    uint64_t pa = user_to_phys_checked(as, va, VMM_ACCESS_WRITE);
    if (pa == 0) {
      if (!vmm_handle_cow_fault(mutable_as, va)) { return false; }
      pa = user_to_phys_checked(as, va, VMM_ACCESS_WRITE);
      if (pa == 0) { return false; }
    }
    kmemcpy((void *)(uintptr_t)(as->hhdm_offset + pa), s + done, chunk);
    done += chunk;
  }
  return true;
}

bool vmm_copy_from_user(const struct user_address_space *as, void *dst, uint64_t src, size_t len) {
  if (len == 0) { return true; }
  uint64_t end;
  if (!checked_add(src, len - 1, &end)) { return false; }
  if (src / PAGE_SIZE != end / PAGE_SIZE && !vmm_user_range_accessible(as, src, len, VMM_ACCESS_READ)) {
    return false;
  }

  uint8_t *d = dst;
  size_t done = 0;
  while (done < len) {
    uint64_t va = src + done;
    uint64_t page_off = va & (PAGE_SIZE - 1);
    size_t chunk = PAGE_SIZE - page_off;
    if (chunk > len - done) { chunk = len - done; }

    uint64_t pa = user_to_phys_checked(as, va, VMM_ACCESS_READ);
    if (pa == 0) { return false; }
    kmemcpy(d + done, (const void *)(uintptr_t)(as->hhdm_offset + pa), chunk);
    done += chunk;
  }
  return true;
}
