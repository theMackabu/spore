#include "mm/pmm.h"

#include "mem.h"

#define PMM_MAX_PHYS (2ull * 1024 * 1024 * 1024)
#define PMM_MAX_PAGES (PMM_MAX_PHYS / PAGE_SIZE)
#define BITS_PER_WORD 64
#define PMM_BITMAP_WORDS (PMM_MAX_PAGES / BITS_PER_WORD)

static uint64_t *hhdm_base;
static uint64_t bitmap[PMM_BITMAP_WORDS];
static uint16_t refcounts[PMM_MAX_PAGES];
static uint64_t total_page_count;
static uint64_t free_page_count;
static uint64_t next_free_page;
static struct pmm_stats stats;

static uint64_t align_down(uint64_t value, uint64_t align) {
  return value & ~(align - 1);
}

static uint64_t align_up(uint64_t value, uint64_t align) {
  return (value + align - 1) & ~(align - 1);
}

static void set_used(uint64_t page) {
  uint64_t mask = 1ull << (page % BITS_PER_WORD);
  uint64_t *word = &bitmap[page / BITS_PER_WORD];
  if ((*word & mask) == 0) {
    *word |= mask;
    if (free_page_count > 0) { --free_page_count; }
  }
  if (refcounts[page] == 0) { refcounts[page] = 1; }
}

static void set_free(uint64_t page) {
  uint64_t mask = 1ull << (page % BITS_PER_WORD);
  uint64_t *word = &bitmap[page / BITS_PER_WORD];
  if ((*word & mask) != 0) {
    *word &= ~mask;
    ++free_page_count;
  }
  refcounts[page] = 0;
}

static uint64_t alloc_page_in_range(uint64_t start_page, uint64_t end_page) {
  if (end_page > PMM_MAX_PAGES) { end_page = PMM_MAX_PAGES; }
  if (start_page >= end_page) { return 0; }

  uint64_t first_word = start_page / BITS_PER_WORD;
  uint64_t last_word = (end_page - 1) / BITS_PER_WORD;
  for (uint64_t word_index = first_word; word_index <= last_word; ++word_index) {
    ++stats.bitmap_words_scanned;
    uint64_t free_bits = ~bitmap[word_index];
    if (word_index == first_word) {
      uint64_t below = start_page % BITS_PER_WORD;
      if (below != 0) { free_bits &= UINT64_MAX << below; }
    }
    if (word_index == last_word) {
      uint64_t above = end_page % BITS_PER_WORD;
      if (above != 0) { free_bits &= (1ull << above) - 1; }
    }
    if (free_bits == 0) { continue; }
    uint64_t page = word_index * BITS_PER_WORD + (uint64_t)__builtin_ctzll(free_bits);
    set_used(page);
    next_free_page = page + 1;
    return page * PAGE_SIZE;
  }
  return 0;
}

void pmm_init(uint64_t hhdm_offset, const struct spore_memmap_entry *memmap, uint32_t memmap_count) {
  hhdm_base = (uint64_t *)(uintptr_t)hhdm_offset;
  for (size_t i = 0; i < PMM_BITMAP_WORDS; ++i) {
    bitmap[i] = UINT64_MAX;
  }
  for (size_t i = 0; i < PMM_MAX_PAGES; ++i) {
    refcounts[i] = 0;
  }
  total_page_count = 0;
  free_page_count = 0;
  next_free_page = 0x100000 / PAGE_SIZE;
  stats = (struct pmm_stats){0};

  for (uint32_t i = 0; i < memmap_count; ++i) {
    const struct spore_memmap_entry *entry = &memmap[i];
    if (entry->type != SPORE_MEMMAP_USABLE) { continue; }

    uint64_t start = align_up(entry->base, PAGE_SIZE);
    uint64_t end = align_down(entry->base + entry->length, PAGE_SIZE);
    if (end > PMM_MAX_PHYS) { end = PMM_MAX_PHYS; }
    for (uint64_t pa = start; pa < end; pa += PAGE_SIZE) {
      ++total_page_count;
      set_free(pa / PAGE_SIZE);
    }
  }

  for (uint64_t pa = 0; pa < 0x100000; pa += PAGE_SIZE) {
    set_used(pa / PAGE_SIZE);
  }
}

uint64_t pmm_alloc_page(void) {
  const uint64_t first = 0x100000 / PAGE_SIZE;
  ++stats.alloc_attempts;
  if (next_free_page < first || next_free_page >= PMM_MAX_PAGES) { next_free_page = first; }
  uint64_t pa = alloc_page_in_range(next_free_page, PMM_MAX_PAGES);
  if (pa == 0) { pa = alloc_page_in_range(first, next_free_page); }
  if (pa == 0) {
    ++stats.alloc_failures;
    return 0;
  }
  ++stats.alloc_successes;
  return pa;
}

uint64_t pmm_alloc_zero_page(void) {
  uint64_t pa = pmm_alloc_page();
  if (pa != 0) { kmemset((void *)((uintptr_t)hhdm_base + pa), 0, PAGE_SIZE); }
  return pa;
}

void pmm_free_page(uint64_t pa) {
  if ((pa % PAGE_SIZE) != 0 || pa >= PMM_MAX_PHYS) { return; }

  // v1 is cooperative and uniprocessor; refcounts are intentionally unlocked.
  // Preemptive/SMP v2 must revisit this.
  uint64_t page = pa / PAGE_SIZE;
  if (refcounts[page] == 0) { return; }
  --refcounts[page];
  if (refcounts[page] == 0) {
    set_free(page);
    if (page < next_free_page) { next_free_page = page; }
  }
}

bool pmm_share_page(uint64_t pa) {
  if ((pa % PAGE_SIZE) != 0 || pa >= PMM_MAX_PHYS) { return false; }

  // v1 is cooperative and uniprocessor; refcounts are intentionally unlocked.
  uint64_t page = pa / PAGE_SIZE;
  if (refcounts[page] == 0 || refcounts[page] == UINT16_MAX) { return false; }
  ++refcounts[page];
  return true;
}

bool pmm_is_last_ref(uint64_t pa) {
  return pmm_refcount(pa) == 1;
}

uint16_t pmm_refcount(uint64_t pa) {
  if ((pa % PAGE_SIZE) != 0 || pa >= PMM_MAX_PHYS) { return 0; }
  return refcounts[pa / PAGE_SIZE];
}

uint64_t pmm_total_pages(void) {
  return total_page_count;
}

uint64_t pmm_free_pages(void) {
  return free_page_count;
}

struct pmm_stats pmm_get_stats(void) {
  return stats;
}
