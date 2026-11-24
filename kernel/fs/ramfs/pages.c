#include "internal.h"
#include "lib/mem.h"

#include "mm/table.h"

struct ramfs_backing_page {
  struct ramfs_mem_node *owner;
  uint32_t file_page;
  int next;
  int hash_next;
  uint8_t *data;
};

static struct ramfs_backing_page *backing_pages;
static int *backing_page_buckets;
static size_t backing_page_capacity;
static size_t backing_page_used_count;
static int backing_page_free_head = -1;
static uint32_t backing_page_hash(const struct ramfs_mem_node *owner, uint32_t file_page) {
  uint64_t hash = (uint64_t)(uintptr_t)owner ^ file_page;
  hash ^= hash >> 33;
  hash *= 0xff51afd7ed558ccdull;
  hash ^= hash >> 33;
  return (uint32_t)(hash & (backing_page_capacity - 1));
}

static void insert_backing_index(int slot) {
  struct ramfs_backing_page *page = &backing_pages[slot];
  uint32_t bucket = backing_page_hash(page->owner, page->file_page);
  page->hash_next = backing_page_buckets[bucket];
  backing_page_buckets[bucket] = slot;
}

static void remove_backing_index(int slot) {
  struct ramfs_backing_page *page = &backing_pages[slot];
  uint32_t bucket = backing_page_hash(page->owner, page->file_page);
  int *link = &backing_page_buckets[bucket];
  while (*link >= 0) {
    if (*link == slot) {
      *link = page->hash_next;
      break;
    }
    link = &backing_pages[*link].hash_next;
  }
}

static void init_backing_range(size_t first, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    size_t index = first + i;
    backing_pages[index].next = backing_page_free_head;
    backing_page_free_head = (int)index;
  }
}

static bool grow_backing_pages(void) {
  size_t old_capacity = backing_page_capacity;
  size_t new_capacity = old_capacity == 0 ? RAMFS_INITIAL_BACKING_PAGES : old_capacity * 2;
  if (new_capacity <= old_capacity || new_capacity > RAMFS_MAX_BACKING_PAGES) {
    new_capacity = RAMFS_MAX_BACKING_PAGES;
  }
  if (new_capacity <= old_capacity) { return false; }
  struct ramfs_backing_page *new_pages = table_alloc(new_capacity, sizeof(*backing_pages) + sizeof(*backing_page_buckets));
  if (new_pages == NULL) { return false; }
  if (backing_pages != NULL) { kmemcpy(new_pages, backing_pages, old_capacity * sizeof(*backing_pages)); }
  struct ramfs_backing_page *old_pages = backing_pages;
  backing_pages = new_pages;
  backing_page_capacity = new_capacity;
  backing_page_buckets = (int *)(backing_pages + backing_page_capacity);
  for (size_t i = 0; i < backing_page_capacity; ++i) {
    backing_page_buckets[i] = -1;
  }
  for (size_t i = 0; i < old_capacity; ++i) {
    if (backing_pages[i].owner != NULL) { insert_backing_index((int)i); }
  }
  init_backing_range(old_capacity, new_capacity - old_capacity);
  table_free(old_pages, old_capacity, sizeof(*backing_pages) + sizeof(*backing_page_buckets));
  return true;
}

static void free_backing_slot(int slot) {
  --backing_pages[slot].owner->allocated_pages;
  remove_backing_index(slot);
  table_free(backing_pages[slot].data, 1, RAMFS_PAGE_SIZE);
  backing_pages[slot] = (struct ramfs_backing_page){.next = backing_page_free_head};
  backing_page_free_head = slot;
  --backing_page_used_count;
}

void ramfs_reset_pages(void) {
  for (size_t i = 0; i < backing_page_capacity; ++i) {
    if (backing_pages[i].owner != NULL) { table_free(backing_pages[i].data, 1, RAMFS_PAGE_SIZE); }
  }
  table_free(backing_pages, backing_page_capacity, sizeof(*backing_pages) + sizeof(*backing_page_buckets));
  backing_pages = NULL;
  backing_page_buckets = NULL;
  backing_page_capacity = 0;
  backing_page_used_count = 0;
  backing_page_free_head = -1;
}

uint64_t ramfs_backing_used_pages(void) {
  return backing_page_used_count;
}

static int find_backing_page(const struct ramfs *fs, int node, uint32_t file_page) {
  if (backing_page_buckets == NULL) { return -1; }
  const struct ramfs_mem_node *owner = &fs->nodes[node];
  uint32_t bucket = backing_page_hash(owner, file_page);
  for (int slot = backing_page_buckets[bucket]; slot >= 0; slot = backing_pages[slot].hash_next) {
    if (backing_pages[slot].owner == owner && backing_pages[slot].file_page == file_page) { return slot; }
  }
  return -1;
}

static int ensure_backing_page(struct ramfs *fs, int node, uint32_t file_page) {
  int found = find_backing_page(fs, node, file_page);
  if (found >= 0) { return found; }
  if (backing_page_free_head < 0 && !grow_backing_pages()) { return -1; }

  int slot = backing_page_free_head;
  struct ramfs_backing_page *page = &backing_pages[slot];
  page->data = table_alloc(1, RAMFS_PAGE_SIZE);
  if (!page->data) { return -1; }
  backing_page_free_head = page->next;
  page->owner = &fs->nodes[node];
  page->file_page = file_page;
  page->next = fs->nodes[node].first_page;
  fs->nodes[node].first_page = slot;
  insert_backing_index(slot);
  ++page->owner->allocated_pages;
  ++backing_page_used_count;
  return slot;
}

void ramfs_free_node_pages(struct ramfs *fs, int node) {
  int slot = fs->nodes[node].first_page;
  fs->nodes[node].first_page = -1;
  while (slot >= 0) {
    int next = backing_pages[slot].next;
    free_backing_slot(slot);
    slot = next;
  }
}

bool ramfs_truncate(struct ramfs *fs, int index, uint64_t size) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      fs->nodes[index].device != RAMFS_DEV_NONE || !fs->nodes[index].writable || size > RAMFS_MAX_FILE_SIZE) {
    return false;
  }
  if (ramfs_truncate_sealed(fs, index, size)) { return false; }
  uint64_t keep_pages = (size + RAMFS_PAGE_SIZE - 1) / RAMFS_PAGE_SIZE;
  int *link = &fs->nodes[index].first_page;
  while (*link >= 0) {
    int slot = *link;
    if (backing_pages[slot].file_page >= keep_pages) {
      *link = backing_pages[slot].next;
      free_backing_slot(slot);
    } else {
      link = &backing_pages[slot].next;
    }
  }
  if ((size % RAMFS_PAGE_SIZE) != 0) {
    int slot = find_backing_page(fs, index, (uint32_t)(size / RAMFS_PAGE_SIZE));
    if (slot >= 0) {
      uint64_t tail = size % RAMFS_PAGE_SIZE;
      kmemset(backing_pages[slot].data + tail, 0, (size_t)(RAMFS_PAGE_SIZE - tail));
    }
  }
  fs->nodes[index].size = size;
  fs->nodes[index].mtime = ramfs_now_sec;
  fs->nodes[index].ctime = ramfs_now_sec;
  return true;
}

uint64_t ramfs_read(struct ramfs *fs, int index, uint64_t off, void *dst, uint64_t len) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      fs->nodes[index].device != RAMFS_DEV_NONE || off >= fs->nodes[index].size) {
    return 0;
  }
  uint64_t n = fs->nodes[index].size - off;
  if (n > len) { n = len; }
  if (!fs->nodes[index].writable) {
    const uint8_t *src = fs->nodes[index].ro_data;
    kmemcpy(dst, src + off, (size_t)n);
    return n;
  }
  uint8_t *out = dst;
  uint64_t done = 0;
  while (done < n) {
    uint64_t pos = off + done;
    uint32_t page_no = (uint32_t)(pos / RAMFS_PAGE_SIZE);
    uint64_t within = pos % RAMFS_PAGE_SIZE;
    uint64_t chunk = RAMFS_PAGE_SIZE - within;
    if (chunk > n - done) { chunk = n - done; }
    int slot = find_backing_page(fs, index, page_no);
    if (slot >= 0) {
      kmemcpy(out + done, backing_pages[slot].data + within, (size_t)chunk);
    } else {
      kmemset(out + done, 0, (size_t)chunk);
    }
    done += chunk;
  }
  return n;
}

static int64_t write_file(struct ramfs *fs, int index, uint64_t off, const void *src, uint64_t len, bool writeback) {
  if (index < 0 || index >= RAMFS_MAX_NODES || !fs->nodes[index].used || fs->nodes[index].is_dir ||
      fs->nodes[index].device != RAMFS_DEV_NONE || !fs->nodes[index].writable || off > RAMFS_MAX_FILE_SIZE ||
      len > RAMFS_MAX_FILE_SIZE - off) {
    return -1;
  }
  const struct ramfs_mem_node *node = &fs->nodes[index];
  if (writeback && off + len > node->size) { return -1; }
  if ((node->seals & RAMFS_F_SEAL_WRITE) || (!writeback && (node->seals & RAMFS_F_SEAL_FUTURE_WRITE)) ||
      ((node->seals & RAMFS_F_SEAL_GROW) && len && off + len > node->size)) { return -1; }
  if (!len) { return 0; }
  const uint8_t *in = src;
  uint64_t done = 0;
  while (done < len) {
    uint64_t pos = off + done;
    uint32_t page_no = (uint32_t)(pos / RAMFS_PAGE_SIZE);
    uint64_t within = pos % RAMFS_PAGE_SIZE;
    uint64_t chunk = RAMFS_PAGE_SIZE - within;
    if (chunk > len - done) { chunk = len - done; }
    if (writeback) {
      static const uint8_t zeros[RAMFS_PAGE_SIZE];
      int slot = find_backing_page(fs, index, page_no);
      const uint8_t *backing = slot < 0 ? zeros : backing_pages[slot].data;
      if (kmemcmp(backing + within, in + done, chunk) == 0) {
        done += chunk;
        continue;
      }
    }
    int slot = ensure_backing_page(fs, index, page_no);
    if (slot < 0) { return -1; }
    kmemcpy(backing_pages[slot].data + within, in + done, (size_t)chunk);
    done += chunk;
  }
  if (off + len > fs->nodes[index].size) { fs->nodes[index].size = off + len; }
  fs->nodes[index].mtime = ramfs_now_sec;
  fs->nodes[index].ctime = ramfs_now_sec;
  return (int64_t)len;
}

int64_t ramfs_write(struct ramfs *fs, int index, uint64_t off, const void *src, uint64_t len) {
  return write_file(fs, index, off, src, len, false);
}

int64_t ramfs_writeback(struct ramfs *fs, int index, uint64_t off, const void *src, uint64_t len) {
  return write_file(fs, index, off, src, len, true);
}
