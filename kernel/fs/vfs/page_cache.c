#include "internal.h"

#include "lib/mem.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

enum {
  VFS_PAGE_SIZE = 4096,
  VFS_PAGE_CACHE_MIN_ENTRIES = 2048,
  VFS_PAGE_CACHE_MAX_ENTRIES = 32768,
  VFS_PAGE_CACHE_MIN_BUCKETS = 512,
  VFS_PAGE_CACHE_MAX_BUCKETS = 4096,
  VFS_PAGE_CACHE_NONE = UINT32_MAX,
  VFS_READAHEAD_PAGES = 2,
};

struct vfs_page_cache_entry {
  bool valid;
  enum vfs_backend backend;
  uint64_t ino;
  uint64_t dev_id;
  uint64_t off;
  uint64_t age;
  uint64_t pa;
  uint32_t hash_next;
};

static struct vfs_page_cache_entry *page_cache;
static uint32_t *page_cache_buckets;
static size_t page_cache_capacity;
static size_t page_cache_bucket_count;
static uint64_t page_cache_clock;
static uint32_t page_cache_evict_hand;

static void *alloc_kernel_table(size_t count, size_t elem_size) {
  uint64_t bytes = count * elem_size;
  uint64_t pages = (bytes + VFS_PAGE_SIZE - 1) / VFS_PAGE_SIZE;
  uint64_t pa = pmm_alloc_contiguous_pages(pages);
  if (pa == 0) { return NULL; }
  void *ptr = (void *)(uintptr_t)(vfs_hhdm_offset + pa);
  kmemset(ptr, 0, pages * VFS_PAGE_SIZE);
  return ptr;
}

static void free_kernel_table(void *ptr, size_t count, size_t elem_size) {
  if (ptr == NULL) { return; }
  uint64_t bytes = count * elem_size;
  uint64_t pages = (bytes + VFS_PAGE_SIZE - 1) / VFS_PAGE_SIZE;
  uint64_t pa = (uint64_t)(uintptr_t)ptr - vfs_hhdm_offset;
  for (uint64_t i = 0; i < pages; ++i) {
    pmm_free_page(pa + i * VFS_PAGE_SIZE);
  }
}

static size_t page_cache_bucket_count_for(size_t entries) {
  if (entries >= VFS_PAGE_CACHE_MAX_ENTRIES) { return VFS_PAGE_CACHE_MAX_BUCKETS; }
  return VFS_PAGE_CACHE_MIN_BUCKETS;
}

void vfs_page_cache_configure(void) {
  if (page_cache != NULL && page_cache_buckets != NULL) { return; }
  for (size_t entries = VFS_PAGE_CACHE_MAX_ENTRIES; entries >= VFS_PAGE_CACHE_MIN_ENTRIES; entries >>= 1) {
    size_t buckets = page_cache_bucket_count_for(entries);
    struct vfs_page_cache_entry *entries_table = alloc_kernel_table(entries, sizeof(*entries_table));
    if (entries_table == NULL) { continue; }
    uint32_t *bucket_table = alloc_kernel_table(buckets, sizeof(*bucket_table));
    if (bucket_table == NULL) {
      free_kernel_table(entries_table, entries, sizeof(*entries_table));
      continue;
    }
    page_cache = entries_table;
    page_cache_buckets = bucket_table;
    page_cache_capacity = entries;
    page_cache_bucket_count = buckets;
    return;
  }
}

bool vfs_node_cacheable(const struct vfs_node *node) {
  return node != NULL && !node->is_dir && node->device == RAMFS_DEV_NONE &&
         (node->backend == VFS_RAMFS || node->backend == VFS_EXT2);
}

static uint32_t page_cache_hash(enum vfs_backend backend, uint64_t ino, uint64_t dev_id, uint64_t off) {
  uint64_t h = ((uint64_t)backend * 0x9e3779b185ebca87ull) ^ ino ^ (dev_id << 17) ^ (off >> 12);
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  return (uint32_t)(h & (page_cache_bucket_count - 1));
}

static void page_cache_init_buckets_once(void) {
  static bool initialized;
  if (initialized) { return; }
  if (page_cache == NULL || page_cache_buckets == NULL || page_cache_capacity == 0 || page_cache_bucket_count == 0) {
    return;
  }
  for (size_t i = 0; i < page_cache_bucket_count; ++i) {
    page_cache_buckets[i] = VFS_PAGE_CACHE_NONE;
  }
  for (size_t i = 0; i < page_cache_capacity; ++i) {
    page_cache[i].hash_next = VFS_PAGE_CACHE_NONE;
  }
  initialized = true;
}

static bool cache_key_eq(const struct vfs_page_cache_entry *entry, const struct vfs_node *node, uint64_t off) {
  return entry->valid && entry->backend == node->backend && entry->ino == node->ino && entry->dev_id == node->dev_id &&
         entry->off == off;
}

static struct vfs_page_cache_entry *page_cache_find(const struct vfs_node *node, uint64_t off) {
  if (page_cache == NULL || page_cache_buckets == NULL || page_cache_bucket_count == 0) { return NULL; }
  page_cache_init_buckets_once();
  uint32_t bucket = page_cache_hash(node->backend, node->ino, node->dev_id, off);
  for (uint32_t index = page_cache_buckets[bucket]; index != VFS_PAGE_CACHE_NONE; index = page_cache[index].hash_next) {
    if (cache_key_eq(&page_cache[index], node, off)) {
      page_cache[index].age = ++page_cache_clock;
      return &page_cache[index];
    }
  }
  return NULL;
}

static void page_cache_unlink(size_t index) {
  struct vfs_page_cache_entry *entry = &page_cache[index];
  if (!entry->valid) { return; }
  uint32_t bucket = page_cache_hash(entry->backend, entry->ino, entry->dev_id, entry->off);
  uint32_t *slot = &page_cache_buckets[bucket];
  while (*slot != VFS_PAGE_CACHE_NONE) {
    if (*slot == index) {
      *slot = page_cache[*slot].hash_next;
      break;
    }
    slot = &page_cache[*slot].hash_next;
  }
  entry->hash_next = VFS_PAGE_CACHE_NONE;
}

static void page_cache_clear(size_t index) {
  page_cache_unlink(index);
  if (page_cache[index].valid && page_cache[index].pa != 0) { pmm_free_page(page_cache[index].pa); }
  kmemset(&page_cache[index], 0, sizeof(page_cache[index]));
  page_cache[index].hash_next = VFS_PAGE_CACHE_NONE;
}

static uint64_t page_cache_alloc_page(void) {
  uint64_t pa = pmm_alloc_zero_page();
  if (pa != 0) { return pa; }
  for (size_t i = 0; i < page_cache_capacity; ++i) {
    uint32_t index = (page_cache_evict_hand + (uint32_t)i) % (uint32_t)page_cache_capacity;
    if (!page_cache[index].valid) { continue; }
    page_cache_clear(index);
    pa = pmm_alloc_zero_page();
    if (pa != 0) {
      page_cache_evict_hand = (index + 1) % (uint32_t)page_cache_capacity;
      return pa;
    }
  }
  return 0;
}

static struct vfs_page_cache_entry *page_cache_victim(void) {
  if (page_cache == NULL || page_cache_capacity == 0) { return NULL; }
  page_cache_init_buckets_once();
  for (size_t i = 0; i < page_cache_capacity; ++i) {
    uint32_t index = (page_cache_evict_hand + (uint32_t)i) % (uint32_t)page_cache_capacity;
    if (!page_cache[index].valid) {
      page_cache_evict_hand = (index + 1) % (uint32_t)page_cache_capacity;
      return &page_cache[index];
    }
  }
  uint32_t index = page_cache_evict_hand++ % (uint32_t)page_cache_capacity;
  page_cache_clear(index);
  return &page_cache[index];
}

static void page_cache_link(size_t index) {
  struct vfs_page_cache_entry *entry = &page_cache[index];
  uint32_t bucket = page_cache_hash(entry->backend, entry->ino, entry->dev_id, entry->off);
  entry->hash_next = page_cache_buckets[bucket];
  page_cache_buckets[bucket] = (uint32_t)index;
}

static bool page_cache_load(const struct vfs_node *node, uint64_t off) {
  if (!vfs_node_cacheable(node)) { return false; }
  off &= ~(uint64_t)(VFS_PAGE_SIZE - 1);
  if (page_cache_find(node, off) != NULL) { return true; }
  ++vfs_stat_counters.page_cache_loads;
  struct vfs_page_cache_entry *entry = page_cache_victim();
  if (entry == NULL) { return false; }
  size_t index = (size_t)(entry - page_cache);
  entry->valid = false;
  entry->backend = node->backend;
  entry->ino = node->ino;
  entry->dev_id = node->dev_id;
  entry->off = off;
  entry->age = ++page_cache_clock;
  entry->pa = page_cache_alloc_page();
  if (entry->pa == 0) {
    page_cache_clear(index);
    return false;
  }
  if (vfs_hhdm_offset == 0) {
    page_cache_clear(index);
    return false;
  }
  uint8_t *data = (uint8_t *)(uintptr_t)(entry->pa + vfs_hhdm_offset);
  uint64_t readable = 0;
  if (off < node->size) {
    readable = node->size - off;
    if (readable > VFS_PAGE_SIZE) { readable = VFS_PAGE_SIZE; }
  }
  if (readable != 0 && vfs_read_uncached(node, off, data, readable) != readable) {
    page_cache_clear(index);
    return false;
  }
  entry->valid = true;
  page_cache_link(index);
  return true;
}

bool vfs_read_page_cached(const struct vfs_node *node, uint64_t off, void *dst) {
  if (dst == NULL || !vfs_node_cacheable(node)) { return false; }
  off &= ~(uint64_t)(VFS_PAGE_SIZE - 1);
  if (page_cache_find(node, off) != NULL) {
    ++vfs_stat_counters.page_cache_hits;
  } else {
    ++vfs_stat_counters.page_cache_misses;
  }
  if (!page_cache_load(node, off)) { return false; }
  for (uint64_t i = 1; i <= VFS_READAHEAD_PAGES; ++i) {
    uint64_t ahead = off + i * VFS_PAGE_SIZE;
    if (ahead >= node->size) { break; }
    (void)page_cache_load(node, ahead);
  }
  struct vfs_page_cache_entry *entry = page_cache_find(node, off);
  if (entry == NULL) { return false; }
  kmemcpy(dst, (const void *)(uintptr_t)(vfs_hhdm_offset + entry->pa), VFS_PAGE_SIZE);
  return true;
}

bool vfs_retain_page_cached(const struct vfs_node *node, uint64_t off, uint64_t *pa_out) {
  if (pa_out == NULL || !vfs_node_cacheable(node)) { return false; }
  off &= ~(uint64_t)(VFS_PAGE_SIZE - 1);
  if (page_cache_find(node, off) != NULL) {
    ++vfs_stat_counters.page_cache_hits;
  } else {
    ++vfs_stat_counters.page_cache_misses;
  }
  if (!page_cache_load(node, off)) { return false; }
  for (uint64_t i = 1; i <= VFS_READAHEAD_PAGES; ++i) {
    uint64_t ahead = off + i * VFS_PAGE_SIZE;
    if (ahead >= node->size) { break; }
    (void)page_cache_load(node, ahead);
  }
  struct vfs_page_cache_entry *entry = page_cache_find(node, off);
  if (entry == NULL || entry->pa == 0 || !pmm_share_page(entry->pa)) { return false; }
  *pa_out = entry->pa;
  return true;
}

void vfs_page_cache_invalidate(const struct vfs_node *node) {
  if (node == NULL || page_cache == NULL) { return; }
  bool any = false;
  for (size_t i = 0; i < page_cache_capacity; ++i) {
    if (page_cache[i].valid && page_cache[i].backend == node->backend && page_cache[i].ino == node->ino &&
        page_cache[i].dev_id == node->dev_id) {
      page_cache_clear(i);
      any = true;
    }
  }
  if (any) { ++vfs_stat_counters.page_cache_invalidations; }
}

void vfs_page_cache_invalidate_range(const struct vfs_node *node, uint64_t off, uint64_t len) {
  if (len == 0) { return; }
  if (off > UINT64_MAX - (len - 1) || len > 128 * 1024) {
    vfs_page_cache_invalidate(node);
    return;
  }
  uint64_t first = off / VFS_PAGE_SIZE;
  uint64_t last = (off + len - 1) / VFS_PAGE_SIZE;
  bool any = false;
  for (uint64_t page = first; page <= last; ++page) {
    struct vfs_page_cache_entry *entry = page_cache_find(node, page * VFS_PAGE_SIZE);
    if (entry == NULL) { continue; }
    page_cache_clear((size_t)(entry - page_cache));
    any = true;
  }
  if (any) { ++vfs_stat_counters.page_cache_invalidations; }
}

void vfs_page_cache_stats(struct vfs_stats *stats) {
  uint64_t pages = 0;
  for (size_t i = 0; i < page_cache_capacity; ++i) {
    if (page_cache[i].valid && page_cache[i].pa != 0) { ++pages; }
  }
  stats->page_cache_pages = pages;
  stats->page_cache_capacity = page_cache_capacity;
  stats->page_cache_buckets = page_cache_bucket_count;
}
