#include "mm/vmm.h"

#ifdef SPORE_NATIVE_PORT
#include "arch/bringup/mmu.h"
#define publish_table native_publish_table
#define tlb_flush_va native_flush_va
#define tlb_flush_all native_flush_all
#define tlb_flush_pages native_flush_pages
#else
#include "arch/aarch64/mmu.h"
#define publish_table aarch64_publish_table
#define tlb_flush_va aarch64_tlb_flush_va
#define tlb_flush_all aarch64_tlb_flush_all
#define tlb_flush_pages aarch64_tlb_flush_pages
#endif
#include "lib/mem.h"
#include "mm/pmm.h"
#include "mm/file_page.h"

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
constexpr uint64_t PTE_FILE_CACHE = 1ull << 58;
constexpr uint64_t PTE_ADDR_MASK = 0x0000fffffffff000ull;

#ifdef SPORE_NATIVE_PORT
constexpr unsigned USER_ROOT_ENTRIES = 256;
static bool table_present(uint64_t entry) { return (entry & 1) != 0; }
static uint64_t table_pa(uint64_t entry) { return entry_phys(entry); }
static uint64_t table_link(uint64_t pa) { return table_entry(pa); }

static uint64_t allocate_table(bool leaf) {
  if (!leaf) { return pmm_alloc_zero_page(); }
  uint64_t pa = pmm_alloc_contiguous_pages(2);
  if (pa) { kmemset(pmm_phys_to_virt(pa), 0, 2 * PAGE_SIZE); }
  return pa;
}
static void release_table(uint64_t pa, bool leaf) {
  if (leaf) { pmm_free_page(pa + PAGE_SIZE); }
  pmm_free_page(pa);
}
static uint64_t *software_leaves(uint64_t *hardware) { return hardware + 512; }
static void store_leaf(uint64_t *slot, uint64_t entry) {
  *slot = entry;
  unsigned prot = PAGE_USER;
  if (entry & PTE_AP_USER_RW) { prot |= PAGE_READ; }
  if ((entry & PTE_AP_MASK) == PTE_AP_USER_RW) { prot |= PAGE_WRITE; }
  if (!(entry & PTE_UXN)) { prot |= PAGE_EXEC; }
  slot[-512] = (entry & PTE_VALID) && (prot & (PAGE_READ | PAGE_WRITE | PAGE_EXEC)) ?
      page_entry(entry & PTE_ADDR_MASK, prot) : 0;
}
void vmm_install_user(const struct user_address_space *as) {
  if (active_root() != as->root_pa) { install_root(as->root_pa); }
}
#else
constexpr unsigned USER_ROOT_ENTRIES = 512;
static bool table_present(uint64_t entry) { return (entry & 3) == 3; }
static uint64_t table_pa(uint64_t entry) { return entry & PTE_ADDR_MASK; }
static uint64_t table_link(uint64_t pa) { return pa | PTE_VALID | PTE_TABLE; }
static uint64_t allocate_table(bool leaf) { (void)leaf; return pmm_alloc_zero_page(); }
static void release_table(uint64_t pa, bool leaf) { (void)leaf; pmm_free_page(pa); }
static uint64_t *software_leaves(uint64_t *table) { return table; }
static void store_leaf(uint64_t *slot, uint64_t entry) { *slot = entry; }
#endif

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
    if (!table_present(entry)) {
      *span_shift = shift;
      return NULL;
    }
    table = pt_virt(as, table_pa(entry));
  }
  *span_shift = 21;
  return &software_leaves(table)[pt_index(va, 12)];
}

static uint64_t *leaf_for_va(const struct user_address_space *as, uint64_t va) {
  unsigned shift;
  return va < USER_VA_LIMIT ? walk_to_leaf(as, va, &shift) : NULL;
}

struct leaf_range {
  uint64_t next_page;
  uint64_t limit;
  uint64_t va;
  uint64_t *ptes;
  size_t count;
};

static struct leaf_range leaf_range(uint64_t start, uint64_t end) {
  if (end > USER_VA_LIMIT) { end = USER_VA_LIMIT; }
  if (start >= end) { return (struct leaf_range){0}; }
  return (struct leaf_range){.next_page = start / PAGE_SIZE, .limit = (end - 1) / PAGE_SIZE + 1};
}

static bool next_leaf_range(const struct user_address_space *as, struct leaf_range *range, size_t max_pages) {
  while (range->next_page < range->limit) {
    uint64_t page = range->next_page;
    range->va = page * PAGE_SIZE;
    unsigned shift;
    range->ptes = walk_to_leaf(as, range->va, &shift);
    uint64_t span = 1ull << (shift - 12);
    uint64_t next = (page & ~(span - 1)) + span;
    if (range->ptes != NULL && next - page > max_pages) { next = page + max_pages; }
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
#ifdef SPORE_NATIVE_PORT
  uint64_t *table = pmm_phys_to_virt(root);
  const uint64_t *kernel = pmm_phys_to_virt(active_root());
  kmemcpy(table + USER_ROOT_ENTRIES, kernel + USER_ROOT_ENTRIES, PAGE_SIZE / 2);
#endif
  as->context_id = __atomic_add_fetch(&last_context_id, 1, __ATOMIC_RELAXED);
  as->root_pa = root;
  as->hhdm_offset = hhdm_offset;
  as->freebsd_abi = false;
  as->detached_file_pages = false;
  as->has_buffer_pins = false;
  as->resident_pages = 0;
  as->peak_resident_pages = 0;
  as->arg_start = 0;
  as->arg_end = 0;
  as->brk_base = 0;
  as->brk_current = 0;
  as->mmap_base = USER_MMAP_BASE;
  return true;
}

static uint64_t with_protection(uint64_t entry, uint32_t flags) {
  bool trap_write = (entry & PTE_COW) || ((entry & PTE_FILE_CACHE) && (entry & PTE_AP_MASK) != PTE_AP_USER_RW);
  entry &= ~(PTE_AP_MASK | PTE_UXN | PTE_WRITE_ALLOWED);
  if (!(flags & VMM_USER_EXEC)) { entry |= PTE_UXN; }
  if (flags & VMM_USER_WRITE) {
    entry |= PTE_WRITE_ALLOWED | (trap_write ? PTE_AP_USER_RO : PTE_AP_USER_RW);
  } else if (flags & (VMM_USER_READ | VMM_USER_EXEC)) {
    entry |= PTE_AP_USER_RO;
  }
  return entry;
}

static uint64_t *ensure_leaf(struct user_address_space *as, uint64_t va) {
  uint64_t *table = pt_virt(as, as->root_pa);
  uint64_t *first_missing = NULL;
  uint64_t allocated[3];
  bool leaves[3];
  size_t count = 0;
  for (unsigned shift = 39; shift > 12; shift -= 9) {
    uint64_t *slot = &table[pt_index(va, shift)];
    uint64_t entry = *slot;
    if (!(entry & PTE_VALID)) {
      uint64_t pa = allocate_table(shift == 21);
      if (!pa) {
        while (count) { --count; release_table(allocated[count], leaves[count]); }
        return NULL;
      }
      leaves[count] = shift == 21;
      allocated[count++] = pa;
      entry = table_link(pa);
      if (!first_missing) { first_missing = slot; }
      else { *slot = entry; }
    } else if (!table_present(entry)) { return NULL; }
    table = pt_virt(as, table_pa(entry));
  }
  if (first_missing) { publish_table(first_missing, table_link(allocated[0])); }
  return &software_leaves(table)[pt_index(va, 12)];
}

static bool map_user_page(struct user_address_space *as, uint64_t va, uint64_t pa, uint32_t flags,
                          uint64_t permissions) {
  if (va >= USER_VA_LIMIT || va % PAGE_SIZE || !pa || pa % PAGE_SIZE) { return false; }
  uint64_t *pte = ensure_leaf(as, va);
  if (pte == NULL || (*pte & PTE_VALID)) { return false; }
  uint64_t entry = (pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE | PTE_NG | PTE_AF | PTE_SH_INNER | PTE_PXN | permissions;
  if (flags & VMM_MAP_SHARED) { entry |= PTE_SHARED; }
  if (flags & VMM_MAP_FILE_CACHE) { entry |= PTE_FILE_CACHE; }
  store_leaf(pte, with_protection(entry, flags));
  ++as->resident_pages;
  if (as->resident_pages > as->peak_resident_pages) { as->peak_resident_pages = as->resident_pages; }
  tlb_flush_va(va);
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
  return level == 3 || !table_present(entry);
}

static bool is_writable_leaf(uint64_t entry) {
  return (entry & PTE_AP_MASK) == PTE_AP_USER_RW;
}

static uint64_t leaf_with_ap(uint64_t entry, uint64_t ap) {
  return (entry & ~PTE_AP_MASK) | ap;
}

static void forget_file_writer(uint64_t *pte) {
  if ((*pte & PTE_FILE_CACHE) && is_writable_leaf(*pte)) {
    vmm_file_writer_remove(*pte & PTE_ADDR_MASK, pte);
  }
}

void vmm_write_protect_file_pte(uint64_t *pte, uint64_t va) {
  store_leaf(pte, leaf_with_ap(*pte, PTE_AP_USER_RO));
  tlb_flush_va(va);
}

static bool clone_table(struct user_address_space *dst, struct user_address_space *src, uint64_t dst_table_pa,
                            uint64_t src_table_pa, unsigned level, enum vmm_clone_mode mode) {
  uint64_t *dst_table = pt_virt(dst, dst_table_pa);
  uint64_t *src_table = pt_virt(src, src_table_pa);

  if (level == 3) { dst_table = software_leaves(dst_table); src_table = software_leaves(src_table); }
  for (size_t i = 0; i < (level ? 512u : USER_ROOT_ENTRIES); ++i) {
    uint64_t entry = src_table[i];
    if ((entry & PTE_VALID) == 0) { continue; }

    if (is_leaf_at_level(entry, level)) {
      uint64_t pa = entry & PTE_ADDR_MASK;
      if (((entry & PTE_SHARED) && mode == VMM_SNAPSHOT) ||
          (!(entry & PTE_SHARED) && src->has_buffer_pins && pmm_page_pinned(pa))) {
        uint64_t copy = pmm_alloc_page();
        if (copy == 0) { return false; }
        kmemcpy(pt_virt(dst, copy), pt_virt(src, pa), PAGE_SIZE);
        if (entry & PTE_FILE_CACHE) {
          dst->detached_file_pages = true;
          entry &= ~PTE_FILE_CACHE;
          if (entry & PTE_WRITE_ALLOWED) { entry = leaf_with_ap(entry, PTE_AP_USER_RW); }
        }
        entry = (entry & ~PTE_ADDR_MASK) | copy;
      } else {
        if (!pmm_share_page(pa)) { return false; }
        if ((entry & PTE_FILE_CACHE) && is_writable_leaf(entry)) {
          entry = leaf_with_ap(entry, PTE_AP_USER_RO);
        } else if (!(entry & PTE_SHARED)) {
          entry |= PTE_COW;
          if (is_writable_leaf(entry)) { entry = leaf_with_ap(entry, PTE_AP_USER_RO); }
          store_leaf(&src_table[i], entry);
        }
      }
      store_leaf(&dst_table[i], entry);
      continue;
    }

    uint64_t child_pa = allocate_table(level == 2);
    if (child_pa == 0) { return false; }
    dst_table[i] = table_link(child_pa);
    if (!clone_table(dst, src, child_pa, table_pa(entry), level + 1, mode)) { return false; }
  }
  return true;
}

bool vmm_clone(struct user_address_space *dst, struct user_address_space *src, enum vmm_clone_mode mode) {
  if (!vmm_user_init(dst, src->hhdm_offset)) { return false; }
  dst->freebsd_abi = src->freebsd_abi;
  dst->arg_start = src->arg_start;
  dst->arg_end = src->arg_end;
  dst->brk_base = src->brk_base;
  dst->brk_current = src->brk_current;
  dst->mmap_base = src->mmap_base;
  dst->detached_file_pages = src->detached_file_pages;

  bool ok = clone_table(dst, src, dst->root_pa, src->root_pa, 0, mode);
  if (ok) {
    dst->resident_pages = src->resident_pages;
    dst->peak_resident_pages = src->resident_pages;
  } else {
    vmm_destroy(dst);
  }
  tlb_flush_all();
  return ok;
}

static size_t destroy_table(struct user_address_space *as, uint64_t pa_table, unsigned level) {
  size_t leaves = 0;
  uint64_t *table = pt_virt(as, pa_table);
  if (level == 3) { table = software_leaves(table); }
  for (size_t i = 0; i < (level ? 512u : USER_ROOT_ENTRIES); ++i) {
    uint64_t entry = table[i];
    if ((entry & PTE_VALID) == 0) { continue; }
    uint64_t pa = entry & PTE_ADDR_MASK;
    if (is_leaf_at_level(entry, level)) {
      forget_file_writer(&table[i]);
      pmm_free_page(pa);
      ++leaves;
    } else {
      leaves += destroy_table(as, table_pa(entry), level + 1);
    }
  }
  release_table(pa_table, level == 3);
  return leaves;
}

void vmm_destroy(struct user_address_space *as) {
  if (as->root_pa != 0) {
#ifdef SPORE_NATIVE_PORT
    if (active_root() == as->root_pa) { port_idle_address_space(); }
#endif
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

bool vmm_handle_write_fault(struct user_address_space *as, uint64_t va) {
  uint64_t *pte = leaf_for_va(as, va);
  if (pte && (*pte & (PTE_VALID | PTE_FILE_CACHE | PTE_WRITE_ALLOWED)) ==
             (PTE_VALID | PTE_FILE_CACHE | PTE_WRITE_ALLOWED)) {
    if (!is_writable_leaf(*pte)) {
      if (!vmm_file_writer_add(*pte & PTE_ADDR_MASK, pte, va & ~(uint64_t)(PAGE_SIZE - 1))) { return false; }
      store_leaf(pte, leaf_with_ap(*pte, PTE_AP_USER_RW));
    }
    tlb_flush_va(va);
    return true;
  }
  constexpr uint64_t writable_cow = PTE_VALID | PTE_COW | PTE_WRITE_ALLOWED;
  if (pte == NULL || (*pte & writable_cow) != writable_cow) { return false; }

  uint64_t old_entry = *pte;
  uint64_t old_pa = old_entry & PTE_ADDR_MASK;
  if (pmm_is_last_ref(old_pa)) {
    store_leaf(pte, leaf_with_ap(old_entry & ~PTE_COW, PTE_AP_USER_RW));
    tlb_flush_va(va);
    return true;
  }

  uint64_t new_pa = pmm_alloc_page();
  if (new_pa == 0) { return false; }
  kmemcpy((void *)(uintptr_t)(as->hhdm_offset + new_pa), (const void *)(uintptr_t)(as->hhdm_offset + old_pa),
          PAGE_SIZE);

  store_leaf(pte, 0);
  tlb_flush_va(va);
  uint64_t new_entry = leaf_with_ap((old_entry & ~PTE_ADDR_MASK & ~PTE_COW) | (new_pa & PTE_ADDR_MASK), PTE_AP_USER_RW);
  store_leaf(pte, new_entry);
  pmm_free_page(old_pa);
  tlb_flush_va(va);
  return true;
}

uint64_t vmm_pin_writable(struct user_address_space *as, uint64_t va) {
  uint64_t *pte=leaf_for_va(as,va);
  if (!pte || !(*pte & PTE_VALID) || (*pte & (PTE_FILE_CACHE | PTE_COW)) || !is_writable_leaf(*pte)) { return 0; }
  uint64_t page=*pte & PTE_ADDR_MASK;
  if (!pmm_pin_page(page)) { return 0; }
  as->has_buffer_pins=true;
  return page;
}

uint64_t vmm_user_to_phys(const struct user_address_space *as, uint64_t va) {
  return user_to_phys_checked(as, va, VMM_ACCESS_READ);
}

bool vmm_is_mapped(const struct user_address_space *as, uint64_t va) {
  uint64_t *pte = leaf_for_va(as, va);
  return pte != NULL && (*pte & PTE_VALID) != 0;
}

static uint64_t transfer_pages(struct user_address_space *as, uint64_t from, uint64_t to, uint64_t bytes) {
  struct leaf_range range = leaf_range(from, from + bytes);
  while (next_leaf_range(as, &range, 64)) {
    uint64_t dest = to + (range.va - from);
    uint64_t moved = 0;
    size_t i = 0;
    for (; i < range.count; ++i) {
      if (!(range.ptes[i] & PTE_VALID)) { continue; }
      uint64_t *pte = ensure_leaf(as, dest + i * PAGE_SIZE);
      if (!pte || (*pte & PTE_VALID)) { break; }
      uint64_t entry = range.ptes[i];
      if ((entry & PTE_FILE_CACHE) && is_writable_leaf(entry)) {
        forget_file_writer(&range.ptes[i]);
        entry = leaf_with_ap(entry, PTE_AP_USER_RO);
      }
      store_leaf(pte, entry);
      store_leaf(&range.ptes[i], 0);
      moved |= UINT64_C(1) << i;
    }
    tlb_flush_pages(range.va, moved);
    tlb_flush_pages(dest, moved);
    if (i != range.count) { return range.va - from + i * PAGE_SIZE; }
  }
  return bytes;
}

bool vmm_move_range(struct user_address_space *as, uint64_t from, uint64_t to, uint64_t bytes) {
  if ((from | to | bytes) % PAGE_SIZE || from >= USER_VA_LIMIT || to >= USER_VA_LIMIT ||
      bytes > USER_VA_LIMIT - from || bytes > USER_VA_LIMIT - to ||
      (from < to + bytes && to < from + bytes) || vmm_mapped_pages_in_range(as, to, to + bytes)) { return false; }
  uint64_t moved = transfer_pages(as, from, to, bytes);
  if (moved == bytes) { return true; }
  (void)transfer_pages(as, to, from, moved);
  vmm_unmap_range(as, to, to + bytes);
  return false;
}

struct retired_tables {
  uint64_t pages[64];
  size_t count;
};

static void reclaim_tables(struct user_address_space *as, struct retired_tables *retired) {
  if (!retired->count) { return; }
  tlb_flush_all();
  while (retired->count) {
    uint64_t tagged = retired->pages[--retired->count];
    as->resident_pages -= destroy_table(as, tagged & PTE_ADDR_MASK, tagged & 3);
  }
}

static void unmap_leaves(struct user_address_space *as, uint64_t *table, uint64_t start, uint64_t end) {
  while (start < end) {
    size_t count = (end - start) / PAGE_SIZE;
    if (count > 64) { count = 64; }
    uint64_t *ptes = table + pt_index(start, 12);
    uint64_t removed = 0;
    for (size_t i = 0; i < count; ++i) {
      if (!(ptes[i] & PTE_VALID)) { continue; }
      forget_file_writer(&ptes[i]);
      store_leaf(&ptes[i], ptes[i] & ~PTE_VALID);
      removed |= UINT64_C(1) << i;
    }
    tlb_flush_pages(start, removed);
    as->resident_pages -= (unsigned)__builtin_popcountll(removed);
    while (removed) {
      size_t i = (size_t)__builtin_ctzll(removed);
      pmm_free_page(ptes[i] & PTE_ADDR_MASK);
      ptes[i] = 0;
      removed &= removed - 1;
    }
    start += count * PAGE_SIZE;
  }
}

static bool table_empty(const uint64_t *table) {
  for (size_t i = 0; i < 512; ++i) {
    if (table[i] & PTE_VALID) { return false; }
  }
  return true;
}

static void unmap_tables(struct user_address_space *as, uint64_t *table, unsigned level, uint64_t start,
                         uint64_t end, struct retired_tables *retired) {
  unsigned shift = 39 - level * 9;
  uint64_t span = UINT64_C(1) << shift;
  while (start < end) {
    uint64_t base = start & ~(span - 1);
    uint64_t limit = base + span < end ? base + span : end;
    uint64_t *slot = &table[pt_index(start, shift)];
    if (table_present(*slot)) {
      uint64_t pa = table_pa(*slot);
      bool whole = start == base && limit - start == span;
      if (!whole) {
        uint64_t *child = pt_virt(as, pa);
        if (level == 2) { child = software_leaves(child); unmap_leaves(as, child, start, limit); }
        else { unmap_tables(as, child, level + 1, start, limit, retired); }
        whole = table_empty(child);
      }
      if (whole) {
        *slot = 0;
        retired->pages[retired->count++] = pa | (level + 1);
        if (retired->count == sizeof(retired->pages) / sizeof(*retired->pages)) { reclaim_tables(as, retired); }
      }
    }
    start = limit;
  }
}

void vmm_unmap_range(struct user_address_space *as, uint64_t start, uint64_t end) {
  if (end > USER_VA_LIMIT) { end = USER_VA_LIMIT; }
  if (start >= end) { return; }
  struct retired_tables retired = {0};
  start &= ~(uint64_t)(PAGE_SIZE - 1);
  end = (end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
  unmap_tables(as, pt_virt(as, as->root_pa), 0, start, end, &retired);
  reclaim_tables(as, &retired);
}

void vmm_protect_range(struct user_address_space *as, uint64_t start, uint64_t end, uint32_t flags) {
  struct leaf_range range = leaf_range(start, end);
  while (next_leaf_range(as, &range, 64)) {
    uint64_t changed = 0;
    for (size_t i = 0; i < range.count; ++i) {
      uint64_t entry = range.ptes[i];
      if (!(entry & PTE_VALID)) { continue; }
      uint64_t next = with_protection(entry, flags);
      if (next == entry) { continue; }
      if (!is_writable_leaf(next)) { forget_file_writer(&range.ptes[i]); }
      store_leaf(&range.ptes[i], next);
      changed |= UINT64_C(1) << i;
    }
    tlb_flush_pages(range.va, changed);
  }
}

bool vmm_visit_detached(const struct user_address_space *as, uint64_t start, uint64_t end, vmm_page_visitor visit,
                        void *ctx) {
  struct leaf_range range = leaf_range(start, end);
  while (next_leaf_range(as, &range, 512)) {
    for (size_t i = 0; i < range.count; ++i) {
      uint64_t entry = range.ptes[i];
      if ((entry & (PTE_VALID | PTE_FILE_CACHE)) == PTE_VALID && !visit(ctx, range.va + i * PAGE_SIZE, entry & PTE_ADDR_MASK)) { return false; }
    }
  }
  return true;
}

size_t vmm_mapped_pages_in_range(const struct user_address_space *as, uint64_t start, uint64_t end) {
  size_t pages = 0;
  struct leaf_range range = leaf_range(start, end);
  while (next_leaf_range(as, &range, 512)) {
    for (size_t i = 0; i < range.count; ++i) {
      pages += (range.ptes[i] & PTE_VALID) != 0;
    }
  }
  return pages;
}

static bool user_range_accessible(const struct user_address_space *as, uint64_t va, size_t len, enum vmm_access access,
                                  bool kernel_buffer) {
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
      if (!(entry & PTE_VALID) || !(entry & PTE_ADDR_MASK)) { return false; }
      if (kernel_buffer && access == VMM_ACCESS_WRITE &&
          (entry & (PTE_FILE_CACHE | PTE_WRITE_ALLOWED)) == (PTE_FILE_CACHE | PTE_WRITE_ALLOWED)) { continue; }
      if (!user_entry_allows(entry, access)) { return false; }
    }
    page += count;
  }
  return true;
}

bool vmm_user_range_accessible(const struct user_address_space *as, uint64_t va, size_t len, enum vmm_access access) {
  return user_range_accessible(as, va, len, access, false);
}

bool vmm_user_buffer_accessible(const struct user_address_space *as, uint64_t va, size_t len, enum vmm_access access) {
  return user_range_accessible(as, va, len, access, true);
}

void *vmm_direct_user_buffer(const struct user_address_space *as, uint64_t va, size_t len, enum vmm_access access) {
  if (!len || va >= USER_VA_LIMIT || len > USER_VA_LIMIT - va || access == VMM_ACCESS_EXEC) { return NULL; }
  uint64_t first_pa = 0;
  uint64_t page = va / PAGE_SIZE;
  uint64_t limit = (va + len - 1) / PAGE_SIZE + 1;
  uint64_t expected = 0;
  while (page < limit) {
    const uint64_t *ptes = leaf_for_va(as, page * PAGE_SIZE);
    if (!ptes) { return NULL; }
    size_t count = 512 - page % 512;
    if (count > limit - page) { count = limit - page; }
    for (size_t i = 0; i < count; ++i) {
      uint64_t entry = ptes[i], pa = entry & PTE_ADDR_MASK;
      if ((entry & (PTE_VALID | PTE_COW | PTE_FILE_CACHE)) != PTE_VALID ||
          !pa || !user_entry_allows(entry, access) || (first_pa && pa != expected)) { return NULL; }
      if (!first_pa) { first_pa = pa; }
      expected = pa + PAGE_SIZE;
    }
    page += count;
  }
  return (void *)(uintptr_t)(as->hhdm_offset + first_pa + va % PAGE_SIZE);
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
      if (user_to_phys_checked(as, va, VMM_ACCESS_WRITE) == 0 && !vmm_handle_write_fault(mutable_as, va)) {
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
      if (!vmm_handle_write_fault(mutable_as, va)) { return false; }
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
