#include "mm/vma.h"

#include <assert.h>

void vfs_ref_stub_reset(void);
unsigned vfs_ref_stub_retain_count(void);
unsigned vfs_ref_stub_release_count(void);
unsigned vfs_ref_stub_shared_write_count(void);

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

  struct vma_list gaps;
  vma_list_init(&gaps);
  assert(vma_insert(&gaps, 0x1000, 0x4001000, 0, 0, VMA_ANON));
  assert(vma_insert(&gaps, 0x4002000, 0x4008000, 0, 0, VMA_ANON));
  uint64_t address;
  assert(vma_find_gap(&gaps, 0x2000, 0x400a000, 0x1000, &address) && address == 0x4001000);
  assert(vma_find_gap(&gaps, 0x2000, 0x400a000, 0x2000, &address) && address == 0x4008000);
  assert(!vma_find_gap(&gaps, 0x2000, 0x4009fff, 0x2000, &address));
  assert(!vma_find_gap(&gaps, UINT64_MAX - 1, UINT64_MAX, 0x1000, &address));
  vma_list_destroy(&gaps);

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
  assert(vma_virtual_pages(&heap) == 5);
  assert(vma_lookup(&heap, brk_base + 0x4fff) != NULL);
  assert(vma_lookup(&heap, brk_current) == NULL);
  vma_list_destroy(&heap);

  struct vma_list many;
  vma_list_init(&many);
  size_t target = 6000;
  for (size_t i = 0; i < target; ++i) {
    uint64_t start = 0x100000 + i * 0x2000;
    assert(vma_insert(&many, start, start + 0x1000, 3, 0x22, VMA_ANON));
  }
  assert(vma_count(&many) == target);
  assert(vma_at(&many, target) == NULL);
  uint64_t last = 0x100000 + (target - 1) * 0x2000;
  assert(vma_protect(&many, 0x100000, last + 0x1000, 1));
  assert(vma_count(&many) == target);
  assert(vma_lookup(&many, 0x100000)->prot == 1);
  assert(vma_lookup(&many, last)->prot == 1);
  assert(vma_protect(&many, 0x100000, last + 0x1000, 1));
  struct vma_list many_clone;
  assert(vma_clone(&many_clone, &many));
  assert(vma_count(&many_clone) == target);
  assert(vma_lookup(&many_clone, last)->end == last + 0x1000);
  assert(vma_remove(&many, 0x101000, last));
  assert(vma_count(&many) == 2);
  assert(vma_lookup(&many, last) && !vma_lookup(&many, last - 0x2000));
  assert(vma_lookup(&many_clone, last - 0x2000));
  vma_list_destroy(&many_clone);
  vma_list_destroy(&many);

  struct vma_list adjacent;
  vma_list_init(&adjacent);
  for (size_t i = 0; i < target; ++i) {
    uint64_t start = 0x2000000 + i * 0x1000;
    assert(vma_insert(&adjacent, start, start + 0x1000, i & 1 ? 3 : 1, 0x22, VMA_ANON));
  }
  assert(vma_protect(&adjacent, 0x2000000, 0x2000000 + target * 0x1000, 1));
  assert(vma_count(&adjacent) == 1);
  assert(vma_lookup(&adjacent, 0x2000000)->end == 0x2000000 + target * 0x1000);
  vma_list_destroy(&adjacent);

  vfs_ref_stub_reset();
  struct vma_list files;
  vma_list_init(&files);
  struct vfs_node node = {
    .backend = VFS_RAMFS,
    .ino = 42,
    .is_dir = false,
    .device = RAMFS_DEV_NONE,
    .mode = 0100600u,
    .size = 0x8000,
  };
  enum { MAP_SHARED_TEST = 0x01, MAP_PRIVATE_TEST = 0x02 };
  assert(vma_insert_file(&files, 0x900000, 0x902000, 3, MAP_SHARED_TEST, &node, 0x900000, 0, 0x2000, true));
  assert(vfs_ref_stub_retain_count() == 1);
  assert(vfs_ref_stub_release_count() == 0);
  assert(vfs_ref_stub_shared_write_count() == 1);
  assert(vma_protect(&files, 0x901000, 0x902000, 1));
  assert(vfs_ref_stub_shared_write_count() == 2);
  assert(vma_remove(&files, 0x900000, 0x901000));
  assert(vfs_ref_stub_shared_write_count() == 1);
  vma_list_destroy(&files);
  assert(vfs_ref_stub_retain_count() == vfs_ref_stub_release_count());

  vfs_ref_stub_reset();
  vma_list_init(&files);
  assert(vma_insert_file(&files, 0xa00000, 0xa02000, 3, MAP_SHARED_TEST, &node, 0xa00000, 0, 0x2000, true));
  assert(vfs_ref_stub_shared_write_count() == 1);
  assert(vma_protect(&files, 0xa00000, 0xa02000, 1));
  assert(vma_count(&files) == 1);
  assert(vfs_ref_stub_shared_write_count() == 1);
  assert(vma_protect(&files, 0xa00000, 0xa02000, 3));
  assert(vma_count(&files) == 1);
  assert(vfs_ref_stub_shared_write_count() == 1);
  vma_list_destroy(&files);
  assert(vfs_ref_stub_shared_write_count() == 0);
  assert(vfs_ref_stub_retain_count() == vfs_ref_stub_release_count());

  vfs_ref_stub_reset();
  vma_list_init(&files);
  assert(vma_insert_file(&files, 0xa00000, 0xa01000, 3, MAP_PRIVATE_TEST, &node, 0xa00000, 0, 0x1000, false));
  assert(vfs_ref_stub_shared_write_count() == 0);
  vma_list_destroy(&files);
  assert(vfs_ref_stub_retain_count() == vfs_ref_stub_release_count());

  vma_list_destroy(&clone);
  vma_list_destroy(&list);
  return 0;
}
