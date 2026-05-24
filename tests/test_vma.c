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
  return 0;
}
