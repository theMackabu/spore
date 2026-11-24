#include "internal.h"

#include "lib/mem.h"
#include "mm/pmm.h"
#include "mm/pool.h"
#include "mm/file_page.h"

enum { CACHE_ENTRIES = 32768, PAGE_BUCKETS = 4096, FILE_BUCKETS = 256, VFS_READAHEAD_PAGES = 2 };

struct vfs_page_cache_entry {
  struct vmm_file_page mapped;
  struct vfs_page_cache_entry *dirty_next, **dirty_link;
  int ramfs_index;
  struct vfs_page_cache_entry *hash_next;
  struct vfs_page_cache_entry *file_next;
  struct vfs_page_cache_entry **file_link;
  struct vfs_page_cache_entry *older, *newer;
  uint64_t ino, dev_id, off;
  enum vfs_backend backend;
};

static struct object_pool entries;
static struct vfs_page_cache_entry *pages[PAGE_BUCKETS], *files[FILE_BUCKETS];
static struct vfs_page_cache_entry *oldest, *newest;
static struct vfs_page_cache_entry *dirty_files[FILE_BUCKETS];
static struct vfs_page_cache_entry **dirty_tails[FILE_BUCKETS];

static void mark_dirty(struct vmm_file_page *page);
static size_t evict_pages(size_t count);

void vfs_page_cache_configure(void) {
  if (!entries.limit) {
    (void)pool_init(&entries, sizeof(struct vfs_page_cache_entry), CACHE_ENTRIES);
    vmm_file_pages_init(mark_dirty);
    pmm_set_reclaimer(evict_pages);
  }
}

bool vfs_node_cacheable(const struct vfs_node *node) {
  return node != NULL && !node->is_dir && node->device == RAMFS_DEV_NONE &&
         (node->backend == VFS_RAMFS || node->backend == VFS_EXT2);
}

static size_t file_bucket(uint64_t ino, uint64_t dev) {
  return (ino ^ (ino >> 8) ^ dev) % FILE_BUCKETS;
}

static void mark_dirty(struct vmm_file_page *page) {
  struct vfs_page_cache_entry *entry = (void *)((uint8_t *)page - offsetof(struct vfs_page_cache_entry, mapped));
  if (entry->dirty_link) { return; }
  size_t bucket = file_bucket(entry->ino, entry->dev_id);
  entry->dirty_link = dirty_tails[bucket] ? dirty_tails[bucket] : &dirty_files[bucket];
  *entry->dirty_link = entry;
  dirty_tails[bucket] = &entry->dirty_next;
}

static void mark_clean(struct vfs_page_cache_entry *entry) {
  *entry->dirty_link = entry->dirty_next;
  if (entry->dirty_next) { entry->dirty_next->dirty_link = entry->dirty_link; }
  else { dirty_tails[file_bucket(entry->ino, entry->dev_id)] = entry->dirty_link; }
  entry->dirty_link = NULL;
  entry->dirty_next = NULL;
}

static size_t page_bucket(uint64_t ino, uint64_t dev, uint64_t off) {
  uint64_t h = ino * UINT64_C(0x9e3779b185ebca87) + dev + off / PAGE_SIZE;
  return (h ^ (h >> 32)) % PAGE_BUCKETS;
}

static bool same_file(const struct vfs_page_cache_entry *entry, const struct vfs_node *node) {
  return entry->ino == node->ino && entry->dev_id == node->dev_id && entry->backend == node->backend;
}

static struct vfs_page_cache_entry *page_cache_find(const struct vfs_node *node, uint64_t off) {
  for (struct vfs_page_cache_entry *e = pages[page_bucket(node->ino, node->dev_id, off)]; e; e = e->hash_next) {
    if (e->off == off && same_file(e, node)) { return e; }
  }
  return NULL;
}

static void link_entry(struct vfs_page_cache_entry *entry) {
  size_t bucket = page_bucket(entry->ino, entry->dev_id, entry->off);
  entry->hash_next = pages[bucket];
  pages[bucket] = entry;
  entry->file_link = &files[file_bucket(entry->ino, entry->dev_id)];
  entry->file_next = *entry->file_link;
  if (entry->file_next) { entry->file_next->file_link = &entry->file_next; }
  *entry->file_link = entry;
  entry->older = newest;
  if (newest) { newest->newer = entry; }
  else { oldest = entry; }
  newest = entry;
  vmm_file_page_register(&entry->mapped);
}

static void clear_entry(struct vfs_page_cache_entry *entry) {
  struct vfs_page_cache_entry **slot = &pages[page_bucket(entry->ino, entry->dev_id, entry->off)];
  while (*slot != entry) { slot = &(*slot)->hash_next; }
  *slot = entry->hash_next;
  *entry->file_link = entry->file_next;
  if (entry->file_next) { entry->file_next->file_link = entry->file_link; }
  if (entry->older) { entry->older->newer = entry->newer; }
  else { oldest = entry->newer; }
  if (entry->newer) { entry->newer->older = entry->older; }
  else { newest = entry->older; }
  if (entry->dirty_link) { mark_clean(entry); }
  vmm_file_page_unregister(&entry->mapped);
  pmm_free_page(entry->mapped.pa);
  pool_free(&entries, entry);
}

static size_t evict_pages(size_t count) {
  size_t freed = 0;
  struct vfs_page_cache_entry *entry = oldest;
  while (entry && freed < count) {
    struct vfs_page_cache_entry *next = entry->newer;
    if (!entry->dirty_link && pmm_is_last_ref(entry->mapped.pa)) {
      clear_entry(entry);
      ++freed;
    }
    entry = next;
  }
  return freed;
}

static struct vfs_page_cache_entry *allocate_entry(void) {
  if (!entries.limit) { return NULL; }
  struct vfs_page_cache_entry *entry;
  while (!(entry = pool_alloc(&entries))) {
    if (!evict_pages(1)) { return NULL; }
  }
  entry->mapped.pa = pmm_alloc_page();
  if (!entry->mapped.pa) {
    pool_free(&entries, entry);
    return NULL;
  }
  return entry;
}

static void publish_entry(struct vfs_page_cache_entry *entry, const struct vfs_node *node, uint64_t off) {
  entry->ino = node->ino;
  entry->dev_id = node->dev_id;
  entry->backend = node->backend;
  if (node->backend == VFS_RAMFS) { entry->ramfs_index = node->ramfs.index; }
  entry->off = off;
  link_entry(entry);
}

static struct vfs_page_cache_entry *load_page(const struct vfs_node *node, uint64_t off) {
  struct vfs_page_cache_entry *entry = allocate_entry();
  if (!entry) { return NULL; }
  uint8_t *data = pmm_phys_to_virt(entry->mapped.pa);
  uint64_t len = off < node->size ? node->size - off : 0;
  if (len > PAGE_SIZE) { len = PAGE_SIZE; }
  ++vfs_stat_counters.page_cache_loads;
  if (len && vfs_read_uncached(node, off, data, len) != len) {
    pmm_free_page(entry->mapped.pa);
    pool_free(&entries, entry);
    return NULL;
  }
  if (len < PAGE_SIZE) { kmemset(data + len, 0, PAGE_SIZE - len); }
  publish_entry(entry, node, off);
  return entry;
}

static struct vfs_page_cache_entry *cached_page(const struct vfs_node *node, uint64_t off) {
  struct vfs_page_cache_entry *entry = page_cache_find(node, off);
  if (entry) {
    ++vfs_stat_counters.page_cache_hits;
    return entry;
  }
  ++vfs_stat_counters.page_cache_misses;
  return NULL;
}

static void page_cache_readahead(const struct vfs_node *node, uint64_t off) {
  for (uint64_t i = 1; i <= VFS_READAHEAD_PAGES; ++i) {
    uint64_t ahead = off + i * PAGE_SIZE;
    if (ahead >= node->size) { break; }
    if (!page_cache_find(node, ahead) && !load_page(node, ahead)) { break; }
  }
}

bool vfs_retain_page_cached(const struct vfs_node *node, uint64_t off, uint64_t *pa_out) {
  if (pa_out == NULL || !vfs_node_cacheable(node)) { return false; }
  off &= ~(uint64_t)(PAGE_SIZE - 1);
  struct vfs_page_cache_entry *entry = cached_page(node, off);
  bool miss = !entry;
  if (miss) { entry = load_page(node, off); }
  if (entry == NULL || entry->mapped.pa == 0 || !pmm_share_page(entry->mapped.pa)) { return false; }
  *pa_out = entry->mapped.pa;
  if (miss) { page_cache_readahead(node, off); }
  return true;
}

static size_t read_full_pages(const struct vfs_node *node, uint64_t off, uint8_t *dst, uint64_t len) {
  constexpr size_t MAX_PAGES = 32;
  size_t pages = 1;
  while (pages < MAX_PAGES && pages < len / PAGE_SIZE && !page_cache_find(node, off + pages * PAGE_SIZE)) { ++pages; }
  if (pages < 2) { return 0; }
  struct vfs_page_cache_entry *batch[MAX_PAGES];
  size_t count = 0;
  while (count < pages) {
    struct vfs_page_cache_entry *entry = allocate_entry();
    if (!entry) { break; }
    batch[count++] = entry;
  }
  size_t done = 0;
  while (done < count) {
    size_t end = done + 1;
    while (end < count && batch[end]->mapped.pa == batch[end - 1]->mapped.pa + PAGE_SIZE) { ++end; }
    size_t pos = done * PAGE_SIZE, bytes = (end - done) * PAGE_SIZE;
    uint8_t *data = pmm_phys_to_virt(batch[done]->mapped.pa);
    if (vfs_read_uncached(node, off + pos, data, bytes) != bytes) { break; }
    kmemcpy(dst + pos, data, bytes);
    while (done < end) {
      publish_entry(batch[done], node, off + done * PAGE_SIZE);
      ++done;
    }
  }
  for (size_t i = done; i < count; ++i) {
    pmm_free_page(batch[i]->mapped.pa);
    pool_free(&entries, batch[i]);
  }
  vfs_stat_counters.page_cache_loads += done;
  if (done) { vfs_stat_counters.page_cache_misses += done - 1; }
  return done * PAGE_SIZE;
}

uint64_t vfs_read_cached(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len) {
  uint8_t *out = dst;
  uint64_t done = 0;
  while (done < len) {
    uint64_t page_off = (off + done) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t in_page = (off + done) - page_off;
    struct vfs_page_cache_entry *entry = cached_page(node, page_off);
    if (!entry) {
      if (node->backend == VFS_EXT2 && !in_page && len - done >= 2 * PAGE_SIZE) {
        size_t bytes = read_full_pages(node, page_off, out + done, len - done);
        if (bytes) { done += bytes; continue; }
      }
      entry = load_page(node, page_off);
    }
    if (entry == NULL) { return done + vfs_read_uncached(node, off + done, out + done, len - done); }
    uint64_t chunk = PAGE_SIZE - in_page;
    if (chunk > len - done) { chunk = len - done; }
    kmemcpy(out + done, (const uint8_t *)pmm_phys_to_virt(entry->mapped.pa) + in_page, chunk);
    done += chunk;
  }
  return done;
}

bool vfs_sync_mapped_range(const struct vfs_node *node, uint64_t start, uint64_t end) {
  struct vfs_node current;
  bool refreshed = false;
  struct vfs_page_cache_entry *entry = dirty_files[file_bucket(node->ino, node->dev_id)];
  while (entry) {
    if (!same_file(entry, node) || entry->off < start || entry->off >= end) {
      entry = entry->dirty_next;
      continue;
    }
    if (!refreshed) {
      if (!vfs_refresh(node, &current) || current.ino != node->ino) { return false; }
      refreshed = true;
    }
    struct vfs_page_cache_entry *next = entry;
    size_t bytes = 0;
    do {
      vmm_file_page_rearm(&next->mapped);
      bytes += PAGE_SIZE;
      next = next->dirty_next;
    } while (next && same_file(next, node) && next->off < end && next->off == entry->off + bytes &&
             next->mapped.pa == entry->mapped.pa + bytes && bytes < 128 * 1024);
    if (entry->off < current.size) {
      if (bytes > current.size - entry->off) { bytes = current.size - entry->off; }
      if (vfs_writeback(&current, entry->off, pmm_phys_to_virt(entry->mapped.pa), bytes) != (int64_t)bytes) {
        return false;
      }
    }
    do {
      struct vfs_page_cache_entry *clean = entry;
      entry = entry->dirty_next;
      mark_clean(clean);
    } while (entry != next);
  }
  return true;
}

bool vfs_sync_mapped_pages(void) {
  for (size_t i = 0; i < FILE_BUCKETS; ++i) {
    while (dirty_files[i]) {
      struct vfs_page_cache_entry *entry = dirty_files[i];
      struct vfs_node key = {.backend = entry->backend}, node;
      if (entry->backend == VFS_EXT2) { key.ext2.ino = entry->ino; }
      else {
        key.ramfs.fs = vfs_root_ramfs;
        key.ramfs.index = entry->ramfs_index;
      }
      if (!vfs_refresh(&key, &node) || !same_file(entry, &node)) {
        if (entry->backend != VFS_RAMFS || !pmm_is_last_ref(entry->mapped.pa)) { return false; }
        clear_entry(entry);
        continue;
      }
      if (!vfs_sync_mapped_range(&node, 0, UINT64_MAX)) { return false; }
    }
  }
  return true;
}

void vfs_page_cache_forget(const struct vfs_node *node) {
  struct vfs_page_cache_entry *entry = files[file_bucket(node->ino, node->dev_id)];
  bool any = false;
  while (entry) {
    struct vfs_page_cache_entry *next = entry->file_next;
    if (same_file(entry, node)) {
      vmm_file_page_rearm(&entry->mapped);
      clear_entry(entry);
      any = true;
    }
    entry = next;
  }
  if (any) { ++vfs_stat_counters.page_cache_invalidations; }
}

void vfs_page_cache_truncate(const struct vfs_node *node, uint64_t size) {
  struct vfs_page_cache_entry *e = files[file_bucket(node->ino, node->dev_id)];
  bool any = false;
  while (e) {
    struct vfs_page_cache_entry *next = e->file_next;
    if (same_file(e, node) && (size <= e->off || size - e->off < PAGE_SIZE)) {
      if (e->dirty_link) {
        vmm_file_page_rearm(&e->mapped);
        if (e->off >= size) { mark_clean(e); }
      }
      if (e->off >= size && pmm_is_last_ref(e->mapped.pa)) {
        clear_entry(e);
      } else {
        size_t tail = size <= e->off ? 0 : size - e->off;
        kmemset((uint8_t *)pmm_phys_to_virt(e->mapped.pa) + tail, 0, PAGE_SIZE - tail);
      }
      any = true;
    }
    e = next;
  }
  if (any) { ++vfs_stat_counters.page_cache_invalidations; }
}

void vfs_page_cache_write(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len) {
  const uint8_t *data = src;
  while (len) {
    size_t within = off % PAGE_SIZE;
    size_t chunk = PAGE_SIZE - within;
    if (chunk > len) { chunk = len; }
    struct vfs_page_cache_entry *entry = page_cache_find(node, off - within);
    if (entry) {
      uint8_t *dest = (uint8_t *)pmm_phys_to_virt(entry->mapped.pa) + within;
      if (dest != data) { kmemcpy(dest, data, chunk); }
    }
    len -= chunk;
    off += chunk;
    data += chunk;
  }
}

void vfs_page_cache_stats(struct vfs_stats *stats) {
  stats->page_cache_pages = entries.count;
  stats->page_cache_capacity = entries.limit;
  stats->page_cache_buckets = PAGE_BUCKETS;
}
