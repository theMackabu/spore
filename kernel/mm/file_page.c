#include "mm/file_page.h"
#include "mm/pmm.h"
#include "mm/pool.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#else
static struct object_pool writers;
#endif

struct vmm_file_writer {
  struct vmm_file_writer *next;
  uint64_t *pte;
  uint64_t va;
};

static struct vmm_file_page *buckets[1024];
static void (*on_dirty)(struct vmm_file_page *page);

void vmm_file_pages_init(void (*mark_dirty)(struct vmm_file_page *page)) {
  on_dirty = mark_dirty;
#if !__STDC_HOSTED__
  if (!writers.limit) { pool_init(&writers, sizeof(struct vmm_file_writer), SIZE_MAX); }
#endif
}

static struct vmm_file_page **bucket_for(uint64_t pa) {
  return &buckets[(pa / PAGE_SIZE) % (sizeof(buckets) / sizeof(*buckets))];
}

static struct vmm_file_page **page_slot(uint64_t pa) {
  struct vmm_file_page **slot = bucket_for(pa);
  while (*slot && (*slot)->pa != pa) { slot = &(*slot)->next; }
  return slot;
}

void vmm_file_page_register(struct vmm_file_page *page) {
  struct vmm_file_page **bucket = bucket_for(page->pa);
  page->next = *bucket;
  *bucket = page;
}

void vmm_file_page_unregister(struct vmm_file_page *page) {
  *page_slot(page->pa) = page->next;
}

bool vmm_file_writer_add(uint64_t pa, uint64_t *pte, uint64_t va) {
  struct vmm_file_page *page = *page_slot(pa);
  if (!page) { return false; }
  if (!page->writer_pte) {
    page->writer_pte = pte;
    page->writer_va = va;
    on_dirty(page);
    return true;
  }
#if __STDC_HOSTED__
  struct vmm_file_writer *writer = malloc(sizeof(*writer));
#else
  struct vmm_file_writer *writer = pool_alloc(&writers);
#endif
  if (!writer) { return false; }
  *writer = (struct vmm_file_writer){.next = page->writers, .pte = pte, .va = va};
  page->writers = writer;
  return true;
}

static void remove_writer(struct vmm_file_writer **slot) {
  struct vmm_file_writer *writer = *slot;
  *slot = writer->next;
#if __STDC_HOSTED__
  free(writer);
#else
  pool_free(&writers, writer);
#endif
}

void vmm_file_writer_remove(uint64_t pa, uint64_t *pte) {
  struct vmm_file_page *page = *page_slot(pa);
  if (page->writer_pte == pte) {
    page->writer_pte = NULL;
    return;
  }
  for (struct vmm_file_writer **slot = &page->writers; *slot; slot = &(*slot)->next) {
    if ((*slot)->pte == pte) {
      remove_writer(slot);
      return;
    }
  }
}

void vmm_file_page_rearm(struct vmm_file_page *page) {
  if (page->writer_pte) {
    vmm_write_protect_file_pte(page->writer_pte, page->writer_va);
    page->writer_pte = NULL;
  }
  while (page->writers) {
    vmm_write_protect_file_pte(page->writers->pte, page->writers->va);
    remove_writer(&page->writers);
  }
}
