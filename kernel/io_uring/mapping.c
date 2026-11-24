#include "abi/linux/shared/usercopy.h"
#include "internal.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/memory.h"

static unsigned pages(uint64_t len) {
  return (len + PAGE_SIZE - 1) / PAGE_SIZE;
}
static unsigned ring_pages(const struct uring *r) {
  return pages(sizeof(struct rings) + (r->cq_entries << r->cq_shift) * sizeof(struct cqe) +
               (r->sq_array_offset ? r->entries * 4 : 0));
}

static bool allocate(uint64_t *pa, unsigned n) {
  if (*pa) { return true; }
  *pa = pmm_alloc_contiguous_pages(n);
  if (!*pa) { return false; }
  kmemset(pmm_phys_to_virt(*pa), 0, n * PAGE_SIZE);
  return true;
}

void uring_release(struct uring *r) {
  r->closing = true;
  uring_drop_pending(r);
  uring_drop_resources(r);
  uring_clear_overflow(r);
  if (r->rings) {
    for (unsigned i = 0; i < ring_pages(r); ++i) {
      pmm_free_page(r->rings + i * PAGE_SIZE);
    }
  }
  if (r->sqes) {
    for (unsigned i = 0; i < pages((r->entries << r->sq_shift) * sizeof(struct sqe)); ++i) {
      pmm_free_page(r->sqes + i * PAGE_SIZE);
    }
  }
}

bool uring_prepare_rings(struct uring *r) {
  if (r->rings) { return true; }
  if (!allocate(&r->rings, ring_pages(r))) { return false; }
  struct rings *h = pmm_phys_to_virt(r->rings);
  h->sq_mask = r->entries - 1;
  h->sq_entries = r->entries;
  h->cq_mask = r->cq_entries - 1;
  h->cq_entries = r->cq_entries;
  uring_flush_overflow(r);
  return true;
}

int64_t uring_map(struct uring *r, uint64_t base, uint64_t size, uint64_t off, uint32_t prot, uint32_t flags) {
  bool sqes = off == OFF_SQES;
  if (!sqes && off != 0 && off != OFF_CQ_RING) { return -EINVAL; }
  unsigned n = sqes ? pages((r->entries << r->sq_shift) * sizeof(struct sqe)) : ring_pages(r);
  if (size > (uint64_t)n * PAGE_SIZE || (prot & VMM_USER_EXEC)) { return -EINVAL; }
  uint64_t *backing = sqes ? &r->sqes : &r->rings;
  if (!(sqes ? allocate(backing, n) : uring_prepare_rings(r))) { return -ENOMEM; }
  if (!cell_add_vma_typed(base, base + size, prot, flags, VMA_RING)) { return -ENOMEM; }
  for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
    uint64_t pa = *backing + offset;
    if (pmm_share_page(pa)) {
      if (vmm_map_page(syscall_active_as(), base + offset, pa, prot | VMM_MAP_SHARED)) { continue; }
      pmm_free_page(pa);
    }
    cell_remove_vma(base, base + size);
    return -ENOMEM;
  }
  return base;
}
