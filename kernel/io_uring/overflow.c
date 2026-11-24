#include "internal.h"
#include "mm/pmm.h"
#include "mm/pool.h"

static struct object_pool extras;
struct uring_extra *uring_extra_new(uint64_t data) {
  if (!extras.limit) { pool_init(&extras, sizeof(struct uring_extra), 65536); }
  struct uring_extra *item = pool_alloc(&extras);
  if (item) {
    item->refs = 1;
    item->cqe.data = data;
  }
  return item;
}
void uring_extra_free(struct uring_extra *item) {
  pool_free(&extras, item);
}

void uring_extra_append(struct uring *r, struct uring_extra *item) {
  if (r->overflow_tail) {
    r->overflow_tail->next = item;
  } else {
    r->overflow_head = item;
  }
  r->overflow_tail = item;
  ++r->backlog;
  if (r->rings) {
    struct rings *h = pmm_phys_to_virt(r->rings);
    __atomic_store_n(&h->sq_flags, 2, __ATOMIC_RELEASE);
  }
}

void uring_tag_put(struct uring *r, struct uring_extra *tag) {
  if (!tag || --tag->refs) { return; }
  if (r && !r->closing) {
    uring_extra_append(r, tag);
  } else {
    uring_extra_free(tag);
  }
}

void uring_flush_overflow(struct uring *r) {
  if (!r->rings || !r->overflow_head) { return; }
  while (r->overflow_head) {
    struct uring_extra *p = r->overflow_head;
    if (!uring_post(r, p->cqe.data, p->cqe.result, p->cqe.flags)) { break; }
    r->overflow_head = p->next;
    uring_extra_free(p);
    --r->backlog;
  }
  if (!r->overflow_head) {
    r->overflow_tail = NULL;
    struct rings *h = pmm_phys_to_virt(r->rings);
    __atomic_store_n(&h->sq_flags, 0, __ATOMIC_RELEASE);
  }
}
void uring_clear_overflow(struct uring *r) {
  while (r->overflow_head) {
    struct uring_extra *p = r->overflow_head;
    r->overflow_head = p->next;
    uring_extra_free(p);
  }
}
