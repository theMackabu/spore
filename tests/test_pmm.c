#include "mm/pmm.h"
#include "mm/pool.h"

#include <assert.h>
#include <stdint.h>

static void test_object_pool(void) {
  alignas(PAGE_SIZE) static uint8_t backing[16 * PAGE_SIZE];
  struct spore_memmap_entry memory = {.base = 0x100000, .length = sizeof(backing), .type = SPORE_MEMMAP_USABLE};
  pmm_init((uintptr_t)backing - memory.base, &memory, 1);
  struct object_pool pool = {0};
  uint64_t free_pages = pmm_free_pages();
  uint8_t *files[55];
  assert(pool_init(&pool, 920, 55));
  assert(pmm_free_pages() == free_pages);
  for (size_t i = 0; i < 55; ++i) {
    files[i] = pool_alloc(&pool);
    assert(files[i] != NULL);
    files[i][919] = (uint8_t)(i + 1);
  }
  assert(pool_alloc(&pool) == NULL);
  for (size_t i = 0; i < 55; i += 3) { pool_free(&pool, files[i]); }
  for (size_t i = 0; i < 55; ++i) {
    if (i % 3 == 0) {
      files[i] = pool_alloc(&pool);
      assert(files[i] != NULL && files[i][919] == 0);
    } else {
      assert(files[i][919] == i + 1);
    }
  }
  for (size_t i = 55; i > 0; --i) { pool_free(&pool, files[i - 1]); }
  assert(pmm_free_pages() == free_pages);
}

static void test_refcount_regions(void) {
  alignas(PAGE_SIZE) static uint8_t memory[3 * PAGE_SIZE * 2048];
  struct spore_memmap_entry region = {.base = 16ull << 20, .length = sizeof(memory), .type = SPORE_MEMMAP_USABLE};
  pmm_init((uintptr_t)memory - region.base, &region, 1);
  const size_t n = 2100;
  uint64_t run = pmm_alloc_contiguous_pages(n);
  assert(run);
  for (size_t i = 0; i < n; ++i) {
    uint64_t page = run + i * PAGE_SIZE;
    assert(pmm_share_page(page) && pmm_refcount(page) == 2);
    pmm_free_page(page);
    assert(pmm_is_last_ref(page));
    pmm_free_page(page);
    assert(!pmm_refcount(page));
  }
  assert(pmm_alloc_contiguous_pages(n) == run);
}

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

  struct spore_memmap_entry high_memmap[] = {{
    .base = 9ull * 1024 * 1024 * 1024,
    .length = 2 * PAGE_SIZE,
    .type = SPORE_MEMMAP_USABLE,
  }};
  pmm_init(0, high_memmap, 1);
  assert(pmm_free_pages() == 2);
  assert(pmm_tracked_pages() == ((9ull * 1024 * 1024 * 1024) / PAGE_SIZE) + 2);
  assert(pmm_alloc_page() == 9ull * 1024 * 1024 * 1024);

  struct spore_memmap_entry fragments[] = {
    {.base = 0x100000, .length = 3 * PAGE_SIZE, .type = SPORE_MEMMAP_USABLE},
    {.base = 0x100000 + 61 * PAGE_SIZE, .length = 70 * PAGE_SIZE, .type = SPORE_MEMMAP_USABLE},
  };
  pmm_init(0, fragments, 2);
  assert(pmm_alloc_contiguous_pages(71) == 0);
  uint64_t run = pmm_alloc_contiguous_pages(70);
  assert(run == fragments[1].base);
  assert(pmm_free_pages() == 3);
  for (uint64_t i = 0; i < 70; ++i) {
    assert(pmm_refcount(run + i * PAGE_SIZE) == 1);
  }
  assert(pmm_alloc_contiguous_pages(3) == fragments[0].base);
  assert(pmm_alloc_page() == 0);
  pmm_free_page(run + 63 * PAGE_SIZE);
  pmm_free_page(run + 64 * PAGE_SIZE);
  assert(pmm_alloc_contiguous_pages(2) == run + 63 * PAGE_SIZE);
  for (size_t i = 0; i < 3; ++i) { pmm_free_page(fragments[0].base + i * PAGE_SIZE); }
  assert(pmm_alloc_contiguous_pages(3) == fragments[0].base);

  test_object_pool();
  test_refcount_regions();
  return 0;
}
