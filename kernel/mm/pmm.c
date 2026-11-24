#include "mm/pmm.h"

#include "lib/mem.h"

constexpr int BITS_PER_WORD = 64;

#if __STDC_HOSTED__
#include <stdlib.h>
#endif

static uint64_t *hhdm_base;
static uint64_t *bitmap;
constexpr size_t REFS_PER_PAGE = PAGE_SIZE / sizeof(uint16_t);
static uint16_t **refcounts;
static uint16_t empty_counts[REFS_PER_PAGE];
static uint64_t tracked_page_count;
static uint64_t bitmap_word_count;
static uint64_t total_page_count;
static uint64_t free_page_count;
static uint64_t next_free_page;
static uint64_t first_candidate_word;
static uint64_t metadata_start_page;
static uint64_t metadata_page_count;
static struct pmm_stats stats;
static pmm_reclaim_fn reclaim_pages;

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
  return page < tracked_page_count;
}

static void claim_page(uint64_t page) {
  bitmap[page / BITS_PER_WORD] |= 1ull << (page % BITS_PER_WORD);
  --free_page_count;
}

static void release_page(uint64_t page) {
  bitmap[page / BITS_PER_WORD] &= ~(1ull << (page % BITS_PER_WORD));
  ++free_page_count;
  if (page / BITS_PER_WORD < first_candidate_word) { first_candidate_word = page / BITS_PER_WORD; }
  if (page < next_free_page) { next_free_page = page; }
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
    claim_page(page);
    next_free_page = page + 1;
    return page * PAGE_SIZE;
  }
  return 0;
}

static uint64_t take_page(void) {
  const uint64_t first = 0x100000 / PAGE_SIZE;
  if (next_free_page < first || next_free_page >= tracked_page_count) { next_free_page = first; }
  uint64_t pa = alloc_page_in_range(next_free_page, tracked_page_count);
  if (pa == 0) { pa = alloc_page_in_range(first, next_free_page); }
  return pa;
}

static uint16_t *counts_for(uint64_t page) {
  return refcounts[page / REFS_PER_PAGE];
}

static __attribute__((cold)) uint16_t *allocate_counts(uint64_t page) {
  uint16_t *counts;
#if __STDC_HOSTED__
  if (!hhdm_base) { counts = calloc(1, PAGE_SIZE); }
  else
#endif
  {
    uint64_t pa = take_page();
    if (!pa) { return NULL; }
    counts = kmemset(pmm_phys_to_virt(pa), 0, PAGE_SIZE);
  }
  if (counts) { refcounts[page / REFS_PER_PAGE] = counts; }
  return counts;
}

static inline uint16_t *prepare_counts(uint64_t page) {
  uint16_t *counts = counts_for(page);
  return counts != empty_counts ? counts : allocate_counts(page);
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
#if __STDC_HOSTED__
  if (!hhdm_base && refcounts) {
    for (size_t i = 0; i < div_round_up(tracked_page_count, REFS_PER_PAGE); ++i) {
      if (refcounts[i] != empty_counts) { free(refcounts[i]); }
    }
    free(bitmap);
  }
#endif
  hhdm_base = (uint64_t *)(uintptr_t)hhdm_offset;
  bitmap = NULL;
  refcounts = NULL;
  tracked_page_count = highest_usable_end(memmap, memmap_count) / PAGE_SIZE;
  bitmap_word_count = div_round_up(tracked_page_count, BITS_PER_WORD);
  total_page_count = 0;
  free_page_count = 0;
  next_free_page = 0x100000 / PAGE_SIZE;
  first_candidate_word = next_free_page / BITS_PER_WORD;
  metadata_start_page = 0;
  metadata_page_count = 0;
  stats = (struct pmm_stats){0};
  reclaim_pages = NULL;
  if (tracked_page_count == 0 || bitmap_word_count == 0) { return; }

  uint64_t bitmap_bytes = bitmap_word_count * sizeof(uint64_t);
  uint64_t refcount_bytes = div_round_up(tracked_page_count, REFS_PER_PAGE) * sizeof(*refcounts);
  uint64_t metadata_bytes = align_up(bitmap_bytes + refcount_bytes, PAGE_SIZE);

#if __STDC_HOSTED__
  if (hhdm_offset == 0) {
    bitmap = malloc((size_t)(bitmap_bytes + refcount_bytes));
    if (!bitmap) {
      tracked_page_count = 0;
      bitmap_word_count = 0;
      return;
    }
    refcounts = (uint16_t **)((uint8_t *)bitmap + bitmap_bytes);
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
    refcounts = (uint16_t **)(uintptr_t)((uint8_t *)bitmap + bitmap_bytes);
  }

  kmemset(bitmap, 0xff, bitmap_bytes);
  for (size_t i = 0; i < refcount_bytes / sizeof(*refcounts); ++i) { refcounts[i] = empty_counts; }

  for (uint32_t i = 0; i < memmap_count; ++i) {
    const struct spore_memmap_entry *entry = &memmap[i];
    if (entry->type != SPORE_MEMMAP_USABLE) { continue; }

    uint64_t start = align_up(entry->base, PAGE_SIZE);
    uint64_t end = align_down(entry->base + entry->length, PAGE_SIZE);
    if (end <= start) { continue; }
    uint64_t page = start / PAGE_SIZE;
    uint64_t limit = end / PAGE_SIZE;
    total_page_count += limit - page;
    while (page < limit) {
      uint64_t bit = page % BITS_PER_WORD;
      uint64_t span = BITS_PER_WORD - bit;
      if (span > limit - page) { span = limit - page; }
      uint64_t mask = (UINT64_MAX >> (BITS_PER_WORD - span)) << bit;
      uint64_t *word = &bitmap[page / BITS_PER_WORD];
      free_page_count += (uint64_t)__builtin_popcountll(*word & mask);
      *word &= ~mask;
      page += span;
    }
  }

  for (uint64_t page = 0; page < 0x100000 / PAGE_SIZE && page < tracked_page_count; ++page) {
    if (!(bitmap[page / BITS_PER_WORD] & (1ull << (page % BITS_PER_WORD)))) { claim_page(page); }
  }
  for (uint64_t page = metadata_start_page; page < metadata_start_page + metadata_page_count; ++page) {
    claim_page(page);
  }
  if (metadata_page_count) { (void)prepare_counts(metadata_start_page); }
}

void pmm_set_reclaimer(pmm_reclaim_fn reclaim) {
  reclaim_pages = reclaim;
}

static bool pages_available(uint64_t count) {
  if (count > free_page_count && reclaim_pages) {
    size_t need = count - free_page_count;
    reclaim_pages(need < 32 ? 32 : need);
  }
  return count <= free_page_count;
}

uint64_t pmm_alloc_page(void) {
  ++stats.alloc_attempts;
  if (!pages_available(1)) {
    ++stats.alloc_failures;
    return 0;
  }
  uint64_t pa = take_page();
  if (pa == 0) {
    ++stats.alloc_failures;
    return 0;
  }
  uint64_t page = pa / PAGE_SIZE;
  uint16_t *counts = prepare_counts(page);
  if (!counts) { release_page(page); ++stats.alloc_failures; return 0; }
  counts[page % REFS_PER_PAGE] = 1;
  ++stats.alloc_successes;
  return pa;
}

uint64_t pmm_alloc_zero_page(void) {
  uint64_t pa = pmm_alloc_page();
  if (pa != 0) { kmemset((void *)((uintptr_t)hhdm_base + pa), 0, PAGE_SIZE); }
  return pa;
}

uint64_t pmm_alloc_contiguous_pages(uint64_t count) {
  if (count == 0 || count > tracked_page_count) { return 0; }
  ++stats.alloc_attempts;
  if (!pages_available(count)) {
    ++stats.alloc_failures;
    return 0;
  }

  uint64_t run_len = 0;
  for (uint64_t page = first_candidate_word * BITS_PER_WORD; page < tracked_page_count;) {
    uint64_t used = bitmap[page / BITS_PER_WORD] >> (page % BITS_PER_WORD);
    uint64_t width = BITS_PER_WORD - page % BITS_PER_WORD;
    if (width > tracked_page_count - page) { width = tracked_page_count - page; }
    ++stats.bitmap_words_scanned;
    if (used == UINT64_MAX && page / BITS_PER_WORD == first_candidate_word) {
      ++first_candidate_word;
      page += width;
      continue;
    }
    uint64_t end = page + width;
    while (page < end) {
      bool occupied = (used & 1) != 0;
      uint64_t opposite = occupied ? ~used : used;
      uint64_t span = opposite == 0 ? BITS_PER_WORD : (uint64_t)__builtin_ctzll(opposite);
      if (span > end - page) { span = end - page; }
      run_len = occupied ? 0 : run_len + span;
      page += span;
      if (run_len >= count) {
        uint64_t start = page - run_len;
        for (uint64_t i = 0; i < count; ++i) {
          claim_page(start + i);
        }
        for (uint64_t page = start; page < start + count;) {
          uint16_t *counts = prepare_counts(page);
          if (!counts) {
            for (uint64_t i = 0; i < count; ++i) { release_page(start + i); }
            ++stats.alloc_failures;
            return 0;
          }
          page = (page / REFS_PER_PAGE + 1) * REFS_PER_PAGE;
        }
        for (uint64_t i = start; i < start + count; ++i) { counts_for(i)[i % REFS_PER_PAGE] = 1; }
        next_free_page = start + count;
        ++stats.alloc_successes;
        return start * PAGE_SIZE;
      }
      if (page < end) { used >>= span; }
    }
  }

  ++stats.alloc_failures;
  return 0;
}

void *pmm_phys_to_virt(uint64_t pa) {
  if (hhdm_base == NULL) { return NULL; }
  return (void *)((uintptr_t)hhdm_base + pa);
}

void pmm_free_page(uint64_t pa) {
  if ((pa % PAGE_SIZE) != 0) { return; }

  uint64_t page = pa / PAGE_SIZE;
  if (!valid_page(page)) { return; }
  uint16_t *counts = counts_for(page);
  if (counts[page % REFS_PER_PAGE] && !--counts[page % REFS_PER_PAGE]) { release_page(page); }
}

bool pmm_retain_boot_page(uint64_t pa) {
  uint64_t page = pa / PAGE_SIZE;
  if (!pa || pa % PAGE_SIZE || !valid_page(page) || !(bitmap[page / BITS_PER_WORD] & (1ull << (page % BITS_PER_WORD)))) { return false; }
  uint16_t *counts = prepare_counts(page);
  if (!counts) { return false; }
  if (!counts[page % REFS_PER_PAGE]) { counts[page % REFS_PER_PAGE] = 1; }
  return pmm_share_page(pa);
}

bool pmm_share_page(uint64_t pa) {
  if ((pa % PAGE_SIZE) != 0) { return false; }

  uint64_t page = pa / PAGE_SIZE;
  if (!valid_page(page)) { return false; }
  uint16_t *counts = counts_for(page);
  uint16_t n = counts[page % REFS_PER_PAGE];
  if (!n || n == UINT16_MAX) { return false; }
  counts[page % REFS_PER_PAGE] = n + 1;
  return true;
}

bool pmm_is_last_ref(uint64_t pa) {
  return pmm_refcount(pa) == 1;
}

uint16_t pmm_refcount(uint64_t pa) {
  if ((pa % PAGE_SIZE) != 0) { return 0; }
  if (!valid_page(pa / PAGE_SIZE)) { return 0; }
  uint64_t page = pa / PAGE_SIZE;
  uint16_t *counts = counts_for(page);
  return counts[page % REFS_PER_PAGE];
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

struct pmm_stats pmm_get_stats(void) {
  return stats;
}
