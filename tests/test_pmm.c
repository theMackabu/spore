#include "mm/pmm.h"

#include <assert.h>
#include <stdint.h>

int main(void) {
  struct spore_memmap_entry memmap[] = {{
    .base = 0x100000,
    .length = 16 * PAGE_SIZE,
    .type = SPORE_MEMMAP_USABLE,
  }};

  pmm_init(0, memmap, 1);
  assert(pmm_free_pages() == 16);

  uint64_t first = pmm_alloc_page();
  uint64_t second = pmm_alloc_page();
  struct pmm_stats stats = pmm_get_stats();
  assert(first == 0x100000);
  assert(second == 0x101000);
  assert(stats.alloc_attempts == 2);
  assert(stats.alloc_successes == 2);
  assert(stats.alloc_failures == 0);
  assert(stats.bitmap_words_scanned >= 2);
  assert(pmm_refcount(first) == 1);
  assert(pmm_refcount(second) == 1);
  assert(pmm_is_last_ref(first));
  assert(pmm_free_pages() == 14);

  assert(pmm_share_page(first));
  assert(pmm_refcount(first) == 2);
  assert(!pmm_is_last_ref(first));
  assert(pmm_free_pages() == 14);

  pmm_free_page(first);
  assert(pmm_refcount(first) == 1);
  assert(pmm_is_last_ref(first));
  assert(pmm_free_pages() == 14);

  pmm_free_page(first);
  assert(pmm_refcount(first) == 0);
  assert(pmm_free_pages() == 15);
  assert(pmm_alloc_page() == first);
  assert(pmm_refcount(first) == 1);

  pmm_free_page(0x123);
  pmm_free_page(0x90000000);
  assert(!pmm_share_page(0x123));
  assert(!pmm_share_page(0x90000000));

  stats = pmm_get_stats();
  assert(stats.alloc_attempts == 3);
  assert(stats.alloc_successes == 3);
  assert(stats.alloc_failures == 0);

  return 0;
}
