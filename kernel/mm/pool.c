#include "mm/pool.h"

#include "lib/mem.h"
#include "mm/pmm.h"

struct pool_page {
  struct pool_page *next;
  struct pool_page **prev_link;
  uint64_t pa;
  uint64_t free;
  alignas(max_align_t) unsigned char slots[];
};

static void link_page(struct object_pool *pool, struct pool_page *page) {
  page->next = pool->available;
  page->prev_link = &pool->available;
  if (page->next != NULL) { page->next->prev_link = &page->next; }
  pool->available = page;
}

static void unlink_page(struct pool_page *page) {
  *page->prev_link = page->next;
  if (page->next != NULL) { page->next->prev_link = page->prev_link; }
}

bool pool_init(struct object_pool *pool, size_t size, size_t limit) {
  if (pool->count != 0 || size == 0 || size > PAGE_SIZE - sizeof(struct pool_page) || limit == 0) { return false; }
  size_t align = alignof(max_align_t);
  size_t stride = (size + align - 1) & ~(align - 1);
  size_t slots = (PAGE_SIZE - sizeof(struct pool_page)) / stride;
  if (slots > 64) { slots = 64; }
  *pool = (struct object_pool){.stride = stride, .limit = limit, .free_mask = UINT64_MAX >> (64 - slots)};
  return true;
}

void *pool_alloc(struct object_pool *pool) {
  if (pool->count >= pool->limit) { return NULL; }
  struct pool_page *page = pool->available;
  if (page == NULL) {
    uint64_t pa = pmm_alloc_page();
    if (pa == 0) { return NULL; }
    page = pmm_phys_to_virt(pa);
    page->pa = pa;
    page->free = pool->free_mask;
    link_page(pool, page);
  }
  size_t slot = (size_t)__builtin_ctzll(page->free);
  page->free &= page->free - 1;
  if (page->free == 0) { unlink_page(page); }
  ++pool->count;
  return kmemset(page->slots + slot * pool->stride, 0, pool->stride);
}

void pool_free(struct object_pool *pool, void *object) {
  uintptr_t base = (uintptr_t)object & ~(uintptr_t)(PAGE_SIZE - 1);
  struct pool_page *page = (struct pool_page *)base;
  size_t slot = ((unsigned char *)object - page->slots) / pool->stride;
  if (page->free == 0) { link_page(pool, page); }
  page->free |= UINT64_C(1) << slot;
  --pool->count;
  if (page->free == pool->free_mask) {
    unlink_page(page);
    pmm_free_page(page->pa);
  }
}
