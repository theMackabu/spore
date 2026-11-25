#include "fs/vfs/internal.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#include <assert.h>
#include <string.h>

struct ramfs *vfs_root_ramfs;
struct vfs_stats vfs_stat_counters;
static unsigned char disk[48 * PAGE_SIZE];
static struct vfs_node file = {.backend = VFS_EXT2, .ino = 42, .size = sizeof(disk)};

bool vfs_refresh(const struct vfs_node *node, struct vfs_node *out) {
  if (node->ino != file.ino) { return false; }
  *out = file;
  return true;
}

uint64_t vfs_read_uncached(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len) {
  assert(node->ino == file.ino && off <= node->size && len <= node->size - off);
  memcpy(dst, disk + off, len);
  return len;
}

int64_t vfs_writeback(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len) {
  assert(node->ino == file.ino && off <= sizeof(disk) && len <= sizeof(disk) - off);
  memcpy(disk + off, src, len);
  return (int64_t)len;
}

int main(void) {
  alignas(PAGE_SIZE) static unsigned char memory[80 * PAGE_SIZE];
  struct spore_memmap_entry region = {.base = 0x100000, .length = sizeof(memory), .type = SPORE_MEMMAP_USABLE};
  uintptr_t base = (uintptr_t)memory - region.base;
  pmm_init(base, &region, 1);
  size_t initial_free = pmm_free_pages();
  vfs_page_cache_configure();
  for (size_t i = 0; i < sizeof(disk); ++i) {
    disk[i] = (unsigned char)(i / PAGE_SIZE + i);
  }
  static unsigned char copy[sizeof(disk)];

  struct user_address_space as;
  uint64_t dirty, pinned;
  size_t dirty_off = 4 * PAGE_SIZE;
  assert(vmm_user_init(&as, base) && vfs_retain_page_cached(&file, dirty_off, false, &dirty));
  assert(vmm_map_page(&as, 0x1000, dirty, VMM_USER_READ | VMM_USER_WRITE | VMM_MAP_SHARED | VMM_MAP_FILE_CACHE));
  assert(vmm_copy_to_user(&as, 0x1000, "dirty", 5));
  vmm_destroy(&as);
  assert(vfs_retain_page_cached(&file, 0, true, &pinned));
  assert(vfs_read_cached(&file, 0, copy, sizeof(copy)) == sizeof(copy));
  assert(!memcmp(copy, disk, dirty_off) && !memcmp(copy + dirty_off, "dirty", 5));
  assert(!memcmp(copy + dirty_off + 5, disk + dirty_off + 5, sizeof(copy) - dirty_off - 5));

  size_t free_before = pmm_free_pages(), count = 0;
  uint64_t held[sizeof(memory) / PAGE_SIZE], pa;
  while ((pa = pmm_alloc_page())) {
    assert(count < sizeof(held) / sizeof(*held));
    held[count++] = pa;
    memset(pmm_phys_to_virt(pa), 0xa5, PAGE_SIZE);
  }
  assert(count > free_before && !memcmp(pmm_phys_to_virt(pinned), disk, PAGE_SIZE));
  assert(vfs_sync_mapped_range(&file, 0, file.size) && !memcmp(disk + dirty_off, "dirty", 5));
  while (count) {
    pmm_free_page(held[--count]);
  }
  pmm_free_page(pinned);
  assert(vfs_read_cached(&file, 0, copy, sizeof(copy)) == sizeof(copy) && !memcmp(copy, disk, sizeof(disk)));
  vfs_page_cache_forget(&file);
  file.size = sizeof(disk) - PAGE_SIZE + 1;
  size_t tail = sizeof(disk) - PAGE_SIZE;
  assert(vfs_retain_page_cached(&file, tail - 2 * PAGE_SIZE, true, &pa));
  pmm_free_page(pa);
  assert(vfs_retain_page_cached(&file, tail, true, &pa));
  const unsigned char *last = pmm_phys_to_virt(pa);
  assert(last[0] == disk[tail]);
  for (size_t i = 1; i < PAGE_SIZE; ++i) {
    assert(last[i] == 0);
  }
  pmm_free_page(pa);
  vfs_page_cache_forget(&file);
  assert(pmm_free_pages() == initial_free);
}
