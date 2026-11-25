#include "mm/page_ring.h"
#include "mm/pmm.h"
#include <assert.h>
#include <string.h>

static int fill(void *source, uint8_t *dst, size_t len) {
  memset(dst, *(int *)source, len);
  return 0;
}

static int verify(void *expected, const uint8_t *src, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    assert(src[i] == *(int *)expected);
  }
  return 0;
}

int main(void) {
  alignas(PAGE_SIZE) static uint8_t backing[8 * PAGE_SIZE];
  struct spore_memmap_entry memory = {.base = 0x100000, .length = sizeof(backing), .type = SPORE_MEMMAP_USABLE};
  pmm_init((uintptr_t)backing - memory.base, &memory, 1);
  uint64_t free_pages = pmm_free_pages();
  uint64_t pages[4] = {0}, held[8];
  struct page_ring ring = {.pages = pages, .capacity = sizeof(pages) / sizeof(*pages) * PAGE_SIZE};
  int first = 'A', next = 'B';
  assert(page_ring_write(&ring, PAGE_SIZE, fill, &first) == PAGE_SIZE);
  unsigned n = 0;
  while (pmm_free_pages() > 1) {
    held[n++] = pmm_alloc_page();
  }
  assert(page_ring_write(&ring, 2 * PAGE_SIZE, fill, &next) < 0);
  assert(pmm_free_pages() == 1);
  while (n) {
    pmm_free_page(held[--n]);
  }
  assert(page_ring_read(&ring, ring.capacity, true, verify, &first) == PAGE_SIZE);
  assert(page_ring_write(&ring, 3 * PAGE_SIZE, fill, &next) == 3 * PAGE_SIZE);
  assert(page_ring_read(&ring, PAGE_SIZE, false, verify, &first) == PAGE_SIZE);
  assert(page_ring_read(&ring, 123, false, verify, &next) == 123);
  assert(page_ring_write(&ring, PAGE_SIZE + 123, fill, &first) == PAGE_SIZE + 123);
  assert(page_ring_read(&ring, 3 * PAGE_SIZE - 123, false, verify, &next) == 3 * PAGE_SIZE - 123);
  assert(page_ring_read(&ring, ring.capacity, false, verify, &first) == PAGE_SIZE + 123);
  page_ring_clear(&ring);
  assert(pmm_free_pages() == free_pages);
}
