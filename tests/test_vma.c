#include "mm/vma.h"

#include <assert.h>

int main(void) {
  struct vma_list list;
  vma_list_init(&list);

  assert(vma_insert(&list, 0x1000, 0x5000, 3, 0x22, VMA_ANON));
  assert(!vma_insert(&list, 0x2000, 0x3000, 3, 0x22, VMA_ANON));
  assert(vma_lookup(&list, 0x1000) != 0);
  assert(vma_lookup(&list, 0x4fff) != 0);
  assert(vma_lookup(&list, 0x5000) == 0);

  assert(vma_remove(&list, 0x2000, 0x3000));
  assert(vma_count(&list) == 2);
  assert(vma_lookup(&list, 0x1800) != 0);
  assert(vma_lookup(&list, 0x2800) == 0);
  assert(vma_lookup(&list, 0x3800) != 0);

  assert(vma_insert(&list, 0x2000, 0x3000, 3, 0x22, VMA_ANON));
  assert(vma_count(&list) == 1);

  assert(vma_protect(&list, 0x2000, 0x4000, 1));
  assert(vma_count(&list) == 3);
  assert(vma_lookup(&list, 0x1800)->prot == 3);
  assert(vma_lookup(&list, 0x2800)->prot == 1);
  assert(vma_lookup(&list, 0x4800)->prot == 3);

  struct vma_list clone;
  assert(vma_clone(&clone, &list));
  assert(vma_count(&clone) == 3);
  assert(vma_lookup(&clone, 0x2800)->prot == 1);

  assert(vma_protect(&list, 0x1000, 0x5000, 3));
  assert(vma_count(&list) == 1);

  struct vma_list heap;
  vma_list_init(&heap);
  uint64_t brk_base = 0x800000;
  uint64_t brk_current = brk_base;
  assert(vma_insert(&heap, brk_current, brk_base + 0x3000, 3, 0x22, VMA_ANON));
  brk_current = brk_base + 0x3000;
  assert(vma_count(&heap) == 1);
  assert(vma_lookup(&heap, brk_base) != NULL);
  assert(vma_lookup(&heap, brk_base + 0x2fff) != NULL);

  assert(vma_remove(&heap, brk_base + 0x1000, brk_current));
  brk_current = brk_base + 0x1000;
  assert(vma_count(&heap) == 1);
  assert(vma_lookup(&heap, brk_base + 0x0fff) != NULL);
  assert(vma_lookup(&heap, brk_base + 0x1000) == NULL);

  assert(vma_insert(&heap, brk_current, brk_base + 0x5000, 3, 0x22, VMA_ANON));
  brk_current = brk_base + 0x5000;
  assert(vma_count(&heap) == 1);
  assert(vma_lookup(&heap, brk_base + 0x4fff) != NULL);
  assert(vma_lookup(&heap, brk_current) == NULL);
  vma_list_destroy(&heap);

  struct vma_list many;
  vma_list_init(&many);
  size_t target = (VMA_CHUNK_BYTES / sizeof(struct vma)) + 5;
  for (size_t i = 0; i < target; ++i) {
    uint64_t start = 0x100000 + i * 0x2000;
    assert(vma_insert(&many, start, start + 0x1000, 3, 0x22, VMA_ANON));
  }
  assert(vma_count(&many) == target);
  assert(vma_capacity(&many) >= target);
  struct vma_list many_clone;
  assert(vma_clone(&many_clone, &many));
  assert(vma_count(&many_clone) == target);
  vma_list_destroy(&many_clone);
  vma_list_destroy(&many);
  vma_list_destroy(&clone);
  vma_list_destroy(&list);
  return 0;
}
