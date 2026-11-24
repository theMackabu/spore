#include "mm/pmm.h"
#include "mm/pool.h"

struct pin {
  struct pin *next;
  uint64_t page;
  uint32_t refs;
};

static struct pin *pins[256];
static struct object_pool pool;

static struct pin **slot(uint64_t page) {
  struct pin **p = &pins[page / PAGE_SIZE % 256];
  while (*p && (*p)->page != page) {
    p = &(*p)->next;
  }
  return p;
}

bool pmm_pin_page(uint64_t page) {
  if (!pmm_share_page(page)) { return false; }
  struct pin **p = slot(page);
  if (!*p) {
    if (!pool.limit) { pool_init(&pool, sizeof(struct pin), 65536); }
    *p = pool_alloc(&pool);
    if (!*p) {
      pmm_free_page(page);
      return false;
    }
    (*p)->page = page;
  }
  ++(*p)->refs;
  return true;
}

bool pmm_page_pinned(uint64_t page) {
  return *slot(page) != NULL;
}

void pmm_unpin_page(uint64_t page) {
  struct pin **p = slot(page);
  struct pin *record = *p;
  if (--record->refs == 0) {
    *p = record->next;
    pool_free(&pool, record);
  }
  pmm_free_page(page);
}
