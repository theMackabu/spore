#include "internal.h"
#include "mm/table.h"

constexpr int BLOCK_CACHE_ENTRIES = 32;
constexpr size_t CACHE_BLOCK_BYTES = 4096;

struct block_cache_entry {
  struct ext2_fs *fs;
  uint8_t *data;
  uint64_t age;
  uint32_t block;
  uint8_t next, pins;
  bool dirty;
};

static struct block_cache_entry block_cache[BLOCK_CACHE_ENTRIES];
static uint8_t buckets[BLOCK_CACHE_ENTRIES];
static uint64_t block_cache_clock;

static unsigned bucket_for(uint32_t block) {
  return (block ^ (block >> 5) ^ (block >> 16)) % BLOCK_CACHE_ENTRIES;
}

static void cache_insert(struct block_cache_entry *entry, struct ext2_fs *fs, uint32_t block) {
  uint8_t *head = &buckets[bucket_for(block)];
  entry->fs = fs;
  entry->block = block;
  entry->dirty = false;
  entry->next = *head;
  *head = (uint8_t)(entry - block_cache + 1);
}

static void cache_remove(struct block_cache_entry *entry) {
  uint8_t *link = &buckets[bucket_for(entry->block)];
  uint8_t slot = (uint8_t)(entry - block_cache + 1);
  while (*link && *link != slot) { link = &block_cache[*link - 1].next; }
  if (*link) { *link = entry->next; }
  entry->fs = NULL;
}

static bool cache_flush_entry(struct block_cache_entry *entry) {
  if (!entry->dirty) { return true; }
  if (entry->fs->write == NULL) { return false; }
  if (!entry->fs->write(entry->fs->ctx, (uint64_t)entry->block * entry->fs->block_size, entry->data,
                        entry->fs->block_size)) {
    return false;
  }
  entry->dirty = false;
  ++ext2_stat_counters.block_cache_flushes;
  return true;
}

bool ext2_flush(struct ext2_fs *fs) {
  bool ok = ext2_check_refs(fs, false);
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (block_cache[i].fs == fs && !cache_flush_entry(&block_cache[i])) { ok = false; }
  }
  return ok;
}

bool ext2_drop_cache(struct ext2_fs *fs) {
  if (fs == NULL) { return true; }
  bool ok = true;
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (block_cache[i].fs == fs) {
      if (block_cache[i].pins || !cache_flush_entry(&block_cache[i])) {
        ok = false;
        continue;
      }
      cache_remove(&block_cache[i]);
      table_free(block_cache[i].data, 1, CACHE_BLOCK_BYTES);
      block_cache[i].data = NULL;
    }
  }
  if (ok) { ext2_dir_drop_cache(fs); }
  return ok;
}

uint64_t ext2_cache_used_pages(void) {
  uint64_t pages = 0;
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (block_cache[i].fs != NULL) { ++pages; }
  }
  return pages;
}

static struct block_cache_entry *cache_find(const struct ext2_fs *fs, uint32_t block) {
  uint8_t slot = buckets[bucket_for(block)];
  while (slot) {
    struct block_cache_entry *entry = &block_cache[slot - 1];
    if (entry->block == block && entry->fs == fs) { return entry; }
    slot = entry->next;
  }
  return NULL;
}

static bool cache_victim(struct block_cache_entry **out) {
  struct block_cache_entry *victim = NULL, *empty = NULL;
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    struct block_cache_entry *entry = &block_cache[i];
    if (!entry->fs) { empty = entry; }
    else if (!entry->pins && (!victim || entry->age < victim->age)) { victim = entry; }
  }
  if (empty && (empty->data = table_alloc(1, CACHE_BLOCK_BYTES))) {
    *out = empty;
    return true;
  }
  if (!victim || !cache_flush_entry(victim)) { return false; }
  cache_remove(victim);
  *out = victim;
  return true;
}

static void cache_update_clean(struct ext2_fs *fs, uint32_t block, const void *src) {
  struct block_cache_entry *entry = cache_find(fs, block);
  if (entry == NULL) { return; }
  kmemcpy(entry->data, src, fs->block_size);
  entry->dirty = false;
  entry->age = ++block_cache_clock;
}

bool ext2_cache_range_has_dirty(struct ext2_fs *fs, uint32_t first_block, uint32_t block_count) {
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (!block_cache[i].dirty || block_cache[i].fs != fs) { continue; }
    if (block_cache[i].block >= first_block && block_cache[i].block - first_block < block_count) { return true; }
  }
  return false;
}

static struct block_cache_entry *cache_read(struct ext2_fs *fs, uint32_t block) {
  if (block >= fs->block_count || fs->block_size > CACHE_BLOCK_BYTES) { return NULL; }
  struct block_cache_entry *entry = cache_find(fs, block);
  if (entry == NULL) {
    ++ext2_stat_counters.block_cache_misses;
    if (!cache_victim(&entry)) { return NULL; }
    if (!fs->read(fs->ctx, (uint64_t)block * fs->block_size, entry->data, fs->block_size)) {
      table_free(entry->data, 1, CACHE_BLOCK_BYTES);
      entry->data = NULL;
      return NULL;
    }
    cache_insert(entry, fs, block);
  } else {
    ++ext2_stat_counters.block_cache_hits;
  }
  entry->age = ++block_cache_clock;
  return entry;
}

const uint8_t *ext2_peek_block(struct ext2_fs *fs, uint32_t block) {
  struct block_cache_entry *entry = cache_find(fs, block);
  return entry ? entry->data : NULL;
}

const uint8_t *ext2_cached_block(struct ext2_fs *fs, uint32_t block) {
  struct block_cache_entry *entry = cache_read(fs, block);
  return entry != NULL ? entry->data : NULL;
}

bool ext2_read_block(struct ext2_fs *fs, uint32_t block, void *dst) {
  const uint8_t *data = ext2_cached_block(fs, block);
  if (data == NULL) { return false; }
  kmemcpy(dst, data, fs->block_size);
  return true;
}

bool ext2_write_block(struct ext2_fs *fs, uint32_t block, const void *src) {
  if (block >= fs->block_count || fs->write == NULL || fs->block_size > CACHE_BLOCK_BYTES) { return false; }
  ++ext2_stat_counters.block_cache_writes;
  struct block_cache_entry *entry = cache_find(fs, block);
  if (entry == NULL) {
    if (!cache_victim(&entry)) { return false; }
    cache_insert(entry, fs, block);
  }
  entry->age = ++block_cache_clock;
  kmemcpy(entry->data, src, fs->block_size);
  entry->dirty = true;
  return true;
}

bool ext2_write_blocks_direct(struct ext2_fs *fs, uint32_t first_block, const void *src, uint32_t block_count) {
  if (block_count == 0) { return true; }
  if (fs->write == NULL || first_block >= fs->block_count || block_count > fs->block_count - first_block ||
      block_count > UINT32_MAX / fs->block_size) { return false; }
  uint32_t bytes = block_count * fs->block_size;
  if (!fs->write(fs->ctx, (uint64_t)first_block * fs->block_size, src, bytes)) { return false; }
  const uint8_t *in = src;
  for (uint32_t i = 0; i < block_count; ++i) {
    cache_update_clean(fs, first_block + i, in + (uint64_t)i * fs->block_size);
  }
  ++ext2_stat_counters.block_cache_flushes;
  return true;
}

static bool disk_contains(const struct ext2_fs *fs, uint64_t offset, uint32_t len) {
  uint64_t end = (uint64_t)fs->block_count * fs->block_size;
  return offset <= end && len <= end - offset;
}

bool ext2_begin_edit(struct ext2_fs *fs, uint64_t offset, uint32_t len, struct ext2_block_edit *edit) {
  uint32_t within = offset % fs->block_size;
  if (!fs->write || !disk_contains(fs, offset, len) || len > fs->block_size - within) { return false; }
  struct block_cache_entry *entry = cache_read(fs, (uint32_t)(offset / fs->block_size));
  if (!entry) { return false; }
  ++entry->pins;
  edit->entry = entry;
  edit->bytes = entry->data + within;
  return true;
}

void ext2_end_edit(struct ext2_block_edit *edit, bool changed) {
  if (!edit->entry) { return; }
  if (changed) { edit->entry->dirty = true; ++ext2_stat_counters.block_cache_writes; }
  --edit->entry->pins;
  edit->entry = NULL;
}

bool ext2_read_bytes(struct ext2_fs *fs, uint64_t offset, void *dst, uint32_t len) {
  if (len && !disk_contains(fs, offset, len)) { return false; }
  uint8_t *out = dst;
  while (len > 0) {
    uint32_t block_index = (uint32_t)(offset / fs->block_size);
    uint32_t within = (uint32_t)(offset % fs->block_size);
    uint32_t chunk = fs->block_size - within;
    if (chunk > len) { chunk = len; }
    const uint8_t *block = ext2_cached_block(fs, block_index);
    if (block == NULL) { return false; }
    kmemcpy(out, block + within, chunk);
    out += chunk;
    offset += chunk;
    len -= chunk;
  }
  return true;
}

bool ext2_write_bytes(struct ext2_fs *fs, uint64_t offset, const void *src, uint32_t len) {
  const uint8_t *in = src;
  if (len != 0 && (fs->write == NULL || !disk_contains(fs, offset, len))) { return false; }
  while (len > 0) {
    uint32_t block_index = (uint32_t)(offset / fs->block_size);
    uint32_t within = (uint32_t)(offset % fs->block_size);
    uint32_t chunk = fs->block_size - within;
    if (chunk > len) { chunk = len; }
    if (chunk == fs->block_size) {
      if (!ext2_write_block(fs, block_index, in)) { return false; }
    } else {
      struct block_cache_entry *entry = cache_read(fs, block_index);
      if (entry == NULL) { return false; }
      kmemcpy(entry->data + within, in, chunk);
      entry->dirty = true;
      ++ext2_stat_counters.block_cache_writes;
    }
    in += chunk;
    offset += chunk;
    len -= chunk;
  }
  return true;
}
