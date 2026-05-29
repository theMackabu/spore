#include "bootloader.h"

static uint64_t *new_page_table(void) {
  if (page_table_pool == NULL) { return NULL; }
  if (page_table_pool_used >= page_table_pool_count) {
    uefi_puts(u"spore-boot: page table pool exhausted\r\n");
    return NULL;
  }
  uint64_t *page = page_table_pool[page_table_pool_used++];
  memset(page, 0, PAGE_SIZE);
  return page;
}

static uint64_t pt_index(uint64_t va, uint32_t shift) {
  return (va >> shift) & 0x1ffu;
}

static uint64_t table_desc(uint64_t *table) {
  return (uint64_t)(uintptr_t)table | 0x3ull;
}

static uint64_t *ensure_table(uint64_t *table, uint64_t index) {
  if ((table[index] & 1u) == 0) {
    uint64_t *child = new_page_table();
    if (child == NULL) { return NULL; }
    table[index] = table_desc(child);
  }
  return (uint64_t *)(uintptr_t)(table[index] & 0x0000fffffffff000ull);
}

static int map_page(uint64_t *root_table, uint64_t va, uint64_t pa, uint64_t attrs) {
  uint64_t *l1 = ensure_table(root_table, pt_index(va, 39));
  if (l1 == NULL) { return 0; }
  uint64_t *l2 = ensure_table(l1, pt_index(va, 30));
  if (l2 == NULL) { return 0; }
  uint64_t *l3 = ensure_table(l2, pt_index(va, 21));
  if (l3 == NULL) { return 0; }
  l3[pt_index(va, 12)] = (pa & 0x0000fffffffff000ull) | attrs | 0x3ull;
  return 1;
}

uint64_t current_el(void) {
  uint64_t el;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
  return (el >> 2) & 3u;
}

void install_ttbr1(uint64_t root_pa) {
  uint64_t mair = 0xffull | (0x00ull << 16);
  uint64_t tcr;
  __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
  uint64_t clear = (0x3full << 16) | (1ull << 23) | (3ull << 24) | (3ull << 26) | (3ull << 28) | (3ull << 30);
  tcr &= ~clear;
  tcr |= (16ull << 16) | (1ull << 24) | (1ull << 26) | (3ull << 28) | (2ull << 30);
  __asm__ volatile("msr mair_el1, %0\n"
                   "msr tcr_el1, %1\n"
                   "msr ttbr1_el1, %2\n"
                   "dsb ishst\n"
                   "tlbi vmalle1\n"
                   "dsb ish\n"
                   "isb\n"
                   :
                   : "r"(mair), "r"(tcr), "r"(root_pa)
                   : "memory");
}

int build_page_tables(uint64_t kernel_phys_base, uint64_t kernel_virt_base, uint64_t kernel_span, uint64_t entry,
                      uint64_t hhdm_size, uint64_t *ttbr1_out) {
  (void)entry;
  uint64_t *root_table = new_page_table();
  if (root_table == NULL) {
    uefi_puts(u"spore-boot: root table alloc failed\r\n");
    return 0;
  }
  const uint64_t attr_normal = 0ull << 2;
  const uint64_t af = 1ull << 10;
  const uint64_t sh_inner = 3ull << 8;
  const uint64_t pxn = 1ull << 53;
  const uint64_t uxn = 1ull << 54;
  const uint64_t normal_rw_nx = attr_normal | af | sh_inner | pxn | uxn;
  const uint64_t kernel_rx = attr_normal | af | sh_inner | uxn;
  const uint64_t kernel_rw_nx = attr_normal | af | sh_inner | pxn | uxn;

  uint64_t *hhdm_l1 = new_page_table();
  if (hhdm_l1 == NULL) {
    uefi_puts(u"spore-boot: hhdm l1 alloc failed\r\n");
    return 0;
  }
  root_table[pt_index(HHDM_OFFSET, 39)] = table_desc(hhdm_l1);
  for (uint64_t gb = 0; gb < hhdm_size; gb += 0x40000000ull) {
    uint64_t *l2 = new_page_table();
    if (l2 == NULL) {
      uefi_puts(u"spore-boot: hhdm map failed\r\n");
      return 0;
    }
    hhdm_l1[pt_index(HHDM_OFFSET + gb, 30)] = table_desc(l2);
    for (uint64_t off = 0; off < 0x40000000ull; off += 0x200000ull) {
      uint64_t pa = gb + off;
      l2[pt_index(HHDM_OFFSET + pa, 21)] = (pa & 0x0000ffffffe00000ull) | normal_rw_nx | 0x1ull;
    }
  }

  uint64_t kernel_map = align_up(kernel_span, PAGE_SIZE);
  for (uint64_t off = 0; off < kernel_map; off += PAGE_SIZE) {
    uint64_t attrs = off < 0x100000ull ? kernel_rx : kernel_rw_nx;
    if (!map_page(root_table, kernel_virt_base + off, kernel_phys_base + off, attrs)) {
      uefi_puts(u"spore-boot: kernel map failed\r\n");
      return 0;
    }
  }
  *ttbr1_out = (uint64_t)(uintptr_t)root_table;
  return 1;
}
