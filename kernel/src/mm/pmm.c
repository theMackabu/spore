#include "mm/pmm.h"

#include "mem.h"

#define BITS_PER_WORD 64

#if __STDC_HOSTED__
#include <stdlib.h>
#endif

static uint64_t *hhdm_base;
static uint64_t *bitmap;
static uint16_t *refcounts;
static uint64_t tracked_page_count;
static uint64_t bitmap_word_count;
static uint64_t total_page_count;
static uint64_t free_page_count;
static uint64_t next_free_page;
static uint64_t metadata_start_page;
static uint64_t metadata_page_count;
static struct pmm_stats stats;

static uint64_t align_down(uint64_t value, uint64_t align) {
  return value & ~(align - 1);
}

static uint64_t align_up(uint64_t value, uint64_t align) {
  return (value + align - 1) & ~(align - 1);
}

static uint64_t div_round_up(uint64_t value, uint64_t divisor) {
  return (value + divisor - 1) / divisor;
}

static bool valid_page(uint64_t page) {
  return bitmap != NULL && refcounts != NULL && page < tracked_page_count;
}

static void set_used(uint64_t page) {
  if (!valid_page(page)) { return; }
  uint64_t mask = 1ull << (page % BITS_PER_WORD);
  uint64_t *word = &bitmap[page / BITS_PER_WORD];
  if ((*word & mask) == 0) {
    *word |= mask;
    if (free_page_count > 0) { --free_page_count; }
  }
  if (refcounts[page] == 0) { refcounts[page] = 1; }
}

static void set_free(uint64_t page) {
  if (!valid_page(page)) { return; }
  uint64_t mask = 1ull << (page % BITS_PER_WORD);
  uint64_t *word = &bitmap[page / BITS_PER_WORD];
  if ((*word & mask) != 0) {
    *word &= ~mask;
    ++free_page_count;
  }
  refcounts[page] = 0;
}

static uint64_t alloc_page_in_range(uint64_t start_page, uint64_t end_page) {
  if (end_page > tracked_page_count) { end_page = tracked_page_count; }
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

static uint64_t highest_usable_end(const struct spore_memmap_entry *memmap, uint32_t memmap_count) {
  uint64_t highest = 0;
  for (uint32_t i = 0; i < memmap_count; ++i) {
    const struct spore_memmap_entry *entry = &memmap[i];
    if (entry->type != SPORE_MEMMAP_USABLE) { continue; }
    uint64_t start = align_up(entry->base, PAGE_SIZE);
    uint64_t end = align_down(entry->base + entry->length, PAGE_SIZE);
    if (end > start && end > highest) { highest = end; }
  }
  return highest;
}

static bool find_metadata_region(const struct spore_memmap_entry *memmap, uint32_t memmap_count, uint64_t bytes,
                                 uint64_t *pa_out) {
  for (uint32_t i = 0; i < memmap_count; ++i) {
    const struct spore_memmap_entry *entry = &memmap[i];
    if (entry->type != SPORE_MEMMAP_USABLE) { continue; }
    uint64_t start = align_up(entry->base, PAGE_SIZE);
    uint64_t end = align_down(entry->base + entry->length, PAGE_SIZE);
    if (start < 0x100000ull) { start = 0x100000ull; }
    if (end > start && end - start >= bytes) {
      *pa_out = start;
      return true;
    }
  }
  return false;
}

void pmm_init(uint64_t hhdm_offset, const struct spore_memmap_entry *memmap, uint32_t memmap_count) {
  hhdm_base = (uint64_t *)(uintptr_t)hhdm_offset;
  bitmap = NULL;
  refcounts = NULL;
  tracked_page_count = highest_usable_end(memmap, memmap_count) / PAGE_SIZE;
  bitmap_word_count = div_round_up(tracked_page_count, BITS_PER_WORD);
  total_page_count = 0;
  free_page_count = 0;
  next_free_page = 0x100000 / PAGE_SIZE;
  metadata_start_page = 0;
  metadata_page_count = 0;
  stats = (struct pmm_stats){0};
  if (tracked_page_count == 0 || bitmap_word_count == 0) { return; }

  uint64_t bitmap_bytes = bitmap_word_count * sizeof(uint64_t);
  uint64_t refcount_bytes = tracked_page_count * sizeof(uint16_t);
  uint64_t metadata_bytes = align_up(bitmap_bytes + refcount_bytes, PAGE_SIZE);

#if __STDC_HOSTED__
  if (hhdm_offset == 0) {
    bitmap = malloc((size_t)bitmap_bytes);
    refcounts = malloc((size_t)refcount_bytes);
    if (bitmap == NULL || refcounts == NULL) {
      tracked_page_count = 0;
      bitmap_word_count = 0;
      return;
    }
  } else
#endif
  {
    uint64_t metadata_pa = 0;
    if (!find_metadata_region(memmap, memmap_count, metadata_bytes, &metadata_pa)) {
      tracked_page_count = 0;
      bitmap_word_count = 0;
      return;
    }
    metadata_start_page = metadata_pa / PAGE_SIZE;
    metadata_page_count = metadata_bytes / PAGE_SIZE;
    bitmap = (uint64_t *)(uintptr_t)(hhdm_offset + metadata_pa);
    refcounts = (uint16_t *)(uintptr_t)((uint8_t *)bitmap + bitmap_bytes);
  }

  kmemset(bitmap, 0xff, bitmap_bytes);
  kmemset(refcounts, 0, refcount_bytes);

  for (uint32_t i = 0; i < memmap_count; ++i) {
    const struct spore_memmap_entry *entry = &memmap[i];
    if (entry->type != SPORE_MEMMAP_USABLE) { continue; }

    uint64_t start = align_up(entry->base, PAGE_SIZE);
    uint64_t end = align_down(entry->base + entry->length, PAGE_SIZE);
    for (uint64_t pa = start; pa < end; pa += PAGE_SIZE) {
      ++total_page_count;
      set_free(pa / PAGE_SIZE);
    }
  }

  for (uint64_t pa = 0; pa < 0x100000; pa += PAGE_SIZE) {
    set_used(pa / PAGE_SIZE);
  }
  for (uint64_t page = metadata_start_page; page < metadata_start_page + metadata_page_count; ++page) {
    set_used(page);
  }
}

uint64_t pmm_alloc_page(void) {
  const uint64_t first = 0x100000 / PAGE_SIZE;
  ++stats.alloc_attempts;
  if (next_free_page < first || next_free_page >= tracked_page_count) { next_free_page = first; }
  uint64_t pa = alloc_page_in_range(next_free_page, tracked_page_count);
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

uint64_t pmm_alloc_contiguous_pages(uint64_t count) {
  const uint64_t first = 0x100000 / PAGE_SIZE;
  if (count == 0 || count > tracked_page_count) { return 0; }
  ++stats.alloc_attempts;

  uint64_t run_start = 0;
  uint64_t run_len = 0;
  for (uint64_t page = first; page < tracked_page_count; ++page) {
    ++stats.bitmap_words_scanned;
    if (!valid_page(page) || (bitmap[page / BITS_PER_WORD] & (1ull << (page % BITS_PER_WORD))) != 0) {
      run_len = 0;
      continue;
    }
    if (run_len == 0) { run_start = page; }
    ++run_len;
    if (run_len < count) { continue; }
    for (uint64_t i = 0; i < count; ++i) {
      set_used(run_start + i);
    }
    next_free_page = run_start + count;
    ++stats.alloc_successes;
    return run_start * PAGE_SIZE;
  }

  ++stats.alloc_failures;
  return 0;
}

void pmm_free_page(uint64_t pa) {
  if ((pa % PAGE_SIZE) != 0) { return; }

  // PMM callers run under the big kernel lock; refcounts stay plain scalars.
  uint64_t page = pa / PAGE_SIZE;
  if (!valid_page(page)) { return; }
  if (refcounts[page] == 0) { return; }
  --refcounts[page];
  if (refcounts[page] == 0) {
    set_free(page);
    if (page < next_free_page) { next_free_page = page; }
  }
}

bool pmm_share_page(uint64_t pa) {
  if ((pa % PAGE_SIZE) != 0) { return false; }

  // PMM callers run under the big kernel lock; refcounts stay plain scalars.
  uint64_t page = pa / PAGE_SIZE;
  if (!valid_page(page)) { return false; }
  if (refcounts[page] == 0 || refcounts[page] == UINT16_MAX) { return false; }
  ++refcounts[page];
  return true;
}

bool pmm_is_last_ref(uint64_t pa) {
  return pmm_refcount(pa) == 1;
}

uint16_t pmm_refcount(uint64_t pa) {
  if ((pa % PAGE_SIZE) != 0) { return 0; }
  if (!valid_page(pa / PAGE_SIZE)) { return 0; }
  return refcounts[pa / PAGE_SIZE];
}

uint64_t pmm_total_pages(void) {
  return total_page_count;
}

uint64_t pmm_free_pages(void) {
  return free_page_count;
}

uint64_t pmm_tracked_pages(void) {
  return tracked_page_count;
}

uint64_t pmm_metadata_pages(void) {
  return metadata_page_count;
}

struct pmm_stats pmm_get_stats(void) {
  return stats;
}
