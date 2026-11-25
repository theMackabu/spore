#include "mm/table.h"
#include "lib/mem.h"
#include "mm/pmm.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#endif

void *table_alloc(size_t count, size_t element_size) {
  if (!count || !element_size || count > SIZE_MAX / element_size) { return NULL; }
#if __STDC_HOSTED__
  return calloc(count, element_size);
#else
  size_t bytes = count * element_size;
  size_t pages = 1 + (bytes - 1) / PAGE_SIZE;
  uint64_t pa = pages == 1 ? pmm_alloc_page() : pmm_alloc_contiguous_pages(pages);
  return pa ? kmemset(pmm_phys_to_virt(pa), 0, bytes) : NULL;
#endif
}

void table_free(void *ptr, size_t count, size_t element_size) {
  if (!ptr) { return; }
#if __STDC_HOSTED__
  (void)count;
  (void)element_size;
  free(ptr);
#else
  size_t bytes = count * element_size;
  uint64_t pa = (uintptr_t)ptr - (uintptr_t)pmm_phys_to_virt(0);
  for (size_t i = 0, pages = 1 + (bytes - 1) / PAGE_SIZE; i < pages; ++i) {
    pmm_free_page(pa + i * PAGE_SIZE);
  }
#endif
}
