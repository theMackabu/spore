#include "internal.h"

constexpr int BLOCK_CACHE_ENTRIES = 32;

struct block_cache_entry {
  bool valid;
  bool dirty;
  struct ext2_fs *fs;
  uint32_t block;
  uint64_t age;
  uint8_t data[4096];
};

static struct block_cache_entry block_cache[BLOCK_CACHE_ENTRIES];
static uint64_t block_cache_clock;
static uint32_t ext2_now;
struct ext2_stats ext2_stat_counters;

void ext2_set_now(uint32_t epoch_sec) {
  ext2_now = epoch_sec;
}

uint32_t ext2_now_sec(void) {
  return ext2_now;
}

uint32_t ext2_div_round_up(uint32_t a, uint32_t b) {
  return (a + b - 1) / b;
}

static bool cache_flush_entry(struct block_cache_entry *entry) {
  if (!entry->valid || !entry->dirty) { return true; }
  if (entry->fs == NULL || entry->fs->write == NULL) { return false; }
  if (!entry->fs->write(entry->fs->ctx, (uint64_t)entry->block * entry->fs->block_size, entry->data,
                        entry->fs->block_size)) {
    return false;
  }
  entry->dirty = false;
  ++ext2_stat_counters.block_cache_flushes;
  return true;
}

bool ext2_flush(struct ext2_fs *fs) {
  bool ok = true;
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (block_cache[i].fs == fs && !cache_flush_entry(&block_cache[i])) { ok = false; }
  }
  return ok;
}

bool ext2_drop_cache(struct ext2_fs *fs) {
  bool ok = true;
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (block_cache[i].fs == fs) {
      if (!cache_flush_entry(&block_cache[i])) { ok = false; }
      block_cache[i].valid = false;
      block_cache[i].dirty = false;
      block_cache[i].fs = NULL;
    }
  }
  ext2_dir_drop_cache(fs);
  return ok;
}

static void cache_reset(struct ext2_fs *fs) {
  (void)ext2_drop_cache(fs);
}

uint64_t ext2_cache_used_pages(void) {
  uint64_t pages = 0;
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (block_cache[i].valid) { ++pages; }
  }
  return pages;
}

struct ext2_stats ext2_get_stats(void) {
  return ext2_stat_counters;
}

static struct block_cache_entry *cache_find(struct ext2_fs *fs, uint32_t block) {
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (block_cache[i].valid && block_cache[i].fs == fs && block_cache[i].block == block) { return &block_cache[i]; }
  }
  return NULL;
}

static bool cache_victim(struct block_cache_entry **out) {
  struct block_cache_entry *victim = &block_cache[0];
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (!block_cache[i].valid) {
      *out = &block_cache[i];
      return true;
    }
    if (block_cache[i].age < victim->age) { victim = &block_cache[i]; }
  }
  if (!cache_flush_entry(victim)) { return false; }
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
    if (!block_cache[i].valid || !block_cache[i].dirty || block_cache[i].fs != fs) { continue; }
    if (block_cache[i].block >= first_block && block_cache[i].block - first_block < block_count) { return true; }
  }
  return false;
}

static struct block_cache_entry *cache_read(struct ext2_fs *fs, uint32_t block) {
  if (block >= fs->block_count || fs->block_size > sizeof(block_cache[0].data)) { return NULL; }
  struct block_cache_entry *entry = cache_find(fs, block);
  if (entry == NULL) {
    ++ext2_stat_counters.block_cache_misses;
    if (!cache_victim(&entry)) { return NULL; }
    entry->valid = false;
    if (!fs->read(fs->ctx, (uint64_t)block * fs->block_size, entry->data, fs->block_size)) { return NULL; }
    entry->valid = true;
    entry->dirty = false;
    entry->fs = fs;
    entry->block = block;
  } else {
    ++ext2_stat_counters.block_cache_hits;
  }
  entry->age = ++block_cache_clock;
  return entry;
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
  if (block >= fs->block_count || fs->write == NULL || fs->block_size > sizeof(block_cache[0].data)) { return false; }
  ++ext2_stat_counters.block_cache_writes;
  struct block_cache_entry *entry = cache_find(fs, block);
  if (entry == NULL) {
    if (!cache_victim(&entry)) { return false; }
    entry->valid = true;
    entry->dirty = false;
    entry->fs = fs;
    entry->block = block;
  }
  entry->age = ++block_cache_clock;
  kmemcpy(entry->data, src, fs->block_size);
  entry->dirty = true;
  return true;
}

bool ext2_write_blocks_direct(struct ext2_fs *fs, uint32_t first_block, const void *src, uint32_t block_count) {
  if (block_count == 0) { return true; }
  uint32_t bytes = block_count * fs->block_size;
  if (!fs->write(fs->ctx, (uint64_t)first_block * fs->block_size, src, bytes)) { return false; }
  const uint8_t *in = src;
  for (uint32_t i = 0; i < block_count; ++i) {
    cache_update_clean(fs, first_block + i, in + (uint64_t)i * fs->block_size);
  }
  ++ext2_stat_counters.block_cache_flushes;
  return true;
}

bool ext2_read_bytes(struct ext2_fs *fs, uint64_t offset, void *dst, uint32_t len) {
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
  if (len != 0 && fs->write == NULL) { return false; }
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

bool ext2_read_super(struct ext2_fs *fs, struct ext2_super *out) {
  return fs->read(fs->ctx, EXT2_SUPER_OFFSET, out, sizeof(*out));
}

bool ext2_write_super(struct ext2_fs *fs, const struct ext2_super *sb) {
  return fs->write != NULL && ext2_write_bytes(fs, EXT2_SUPER_OFFSET, sb, sizeof(*sb));
}

bool ext2_read_group_desc(struct ext2_fs *fs, uint32_t group, struct ext2_group_desc *out) {
  if (group >= fs->group_count) { return false; }
  uint64_t table = fs->block_size == 1024 ? 2ull * fs->block_size : fs->block_size;
  return ext2_read_bytes(fs, table + (uint64_t)group * sizeof(*out), out, sizeof(*out));
}

bool ext2_write_group_desc(struct ext2_fs *fs, uint32_t group, const struct ext2_group_desc *in) {
  if (group >= fs->group_count || fs->write == NULL) { return false; }
  uint64_t table = fs->block_size == 1024 ? 2ull * fs->block_size : fs->block_size;
  return ext2_write_bytes(fs, table + (uint64_t)group * sizeof(*in), in, sizeof(*in));
}

static bool ext2plus_header_valid(struct ext2_fs *fs, const struct ext2plus_header *hdr) {
  return hdr->magic == EXT2PLUS_MAGIC && hdr->version == EXT2PLUS_VERSION && hdr->header_size == sizeof(*hdr) &&
         hdr->inode_size == fs->inode_size && hdr->base_ino > fs->inode_count &&
         hdr->base_ino == fs->ext2plus_base_ino && hdr->chunk_size != 0 && hdr->next_index <= hdr->capacity &&
         (hdr->free_head == EXT2PLUS_FREE_NONE || hdr->free_head < hdr->capacity) && hdr->free_count <= hdr->capacity &&
         hdr->static_inodes_used <= EXT2PLUS_STATIC_INODES_BEFORE_DYNAMIC;
}

static void ext2plus_cache_header(struct ext2_fs *fs, const struct ext2plus_header *hdr) {
  fs->ext2plus_capacity = hdr->capacity;
  fs->ext2plus_next_index = hdr->next_index;
  fs->ext2plus_free_head = hdr->free_head;
  fs->ext2plus_free_count = hdr->free_count;
  fs->ext2plus_chunk_size = hdr->chunk_size;
  fs->ext2plus_static_inodes_used = hdr->static_inodes_used;
}

bool ext2plus_load_header(struct ext2_fs *fs, struct ext2plus_header *hdr) {
  *hdr = (struct ext2plus_header){
    .magic = EXT2PLUS_MAGIC,
    .version = EXT2PLUS_VERSION,
    .header_size = sizeof(*hdr),
    .inode_size = fs->inode_size,
    .base_ino = fs->ext2plus_base_ino,
    .capacity = fs->ext2plus_capacity,
    .next_index = fs->ext2plus_next_index,
    .free_head = fs->ext2plus_free_head,
    .free_count = fs->ext2plus_free_count,
    .chunk_size = fs->ext2plus_chunk_size,
    .static_inodes_used = fs->ext2plus_static_inodes_used,
  };
  return ext2plus_header_valid(fs, hdr);
}

bool ext2plus_write_header(struct ext2_fs *fs, const struct ext2plus_header *hdr) {
  if (!ext2plus_header_valid(fs, hdr) || !ext2plus_write_store(fs, 0, hdr, sizeof(*hdr))) { return false; }
  ext2plus_cache_header(fs, hdr);
  return true;
}

static bool ext2plus_mount_init(struct ext2_fs *fs) {
  struct ext2_node store;
  fs->ext2plus = false;
  fs->ext2plus_store_ino = 0;
  fs->ext2plus_base_ino = EXT2PLUS_BASE_INO;
  if (!ext2_lookup(fs, "/.spore-inodes", &store) || !ext2_is_regular(&store)) { return false; }
  fs->ext2plus_store_ino = store.ino;
  fs->ext2plus_store = store;
  fs->ext2plus_store_valid = true;

  struct ext2plus_header hdr;
  uint32_t got = 0;
  bool valid = store.size >= sizeof(hdr) && ext2_read_file(fs, &store, 0, &hdr, sizeof(hdr), &got) &&
               got == sizeof(hdr) && ext2plus_header_valid(fs, &hdr);
  if (!valid && fs->write != NULL) {
    kmemset(&hdr, 0, sizeof(hdr));
    hdr.magic = EXT2PLUS_MAGIC;
    hdr.version = EXT2PLUS_VERSION;
    hdr.header_size = sizeof(hdr);
    hdr.inode_size = fs->inode_size;
    hdr.base_ino = fs->ext2plus_base_ino;
    hdr.free_head = EXT2PLUS_FREE_NONE;
    hdr.chunk_size = EXT2PLUS_CHUNK_INODES;
    valid = ext2plus_write_header(fs, &hdr);
  }
  if (!valid) { return false; }
  ext2plus_cache_header(fs, &hdr);
  fs->ext2plus = true;
  ext2_dir_cache_remove_name(fs, EXT2_ROOT_INO, ".spore-inodes");
  return true;
}

bool ext2_mount_rw(struct ext2_fs *fs, ext2_read_fn read, ext2_write_fn write, void *ctx) {
  struct ext2_super sb;
  if (!read(ctx, EXT2_SUPER_OFFSET, &sb, sizeof(sb))) { return false; }
  if (sb.magic != EXT2_SUPER_MAGIC) { return false; }
  if (sb.log_block_size > 2) { return false; }
  uint32_t supported_incompat = EXT2_INCOMPAT_FILETYPE | EXT2_INCOMPAT_SPORE_EXT2PLUS;
  if ((sb.feature_incompat & ~supported_incompat) != 0) { return false; }

  fs->tmpfile_count = 0;
  fs->read = read;
  fs->write = write;
  fs->ctx = ctx;
  fs->ext2plus = false;
  fs->ext2plus_store_valid = false;
  fs->block_size = 1024u << sb.log_block_size;
  fs->inodes_per_group = sb.inodes_per_group;
  fs->blocks_per_group = sb.blocks_per_group;
  fs->inode_size = sb.inode_size == 0 ? 128 : sb.inode_size;
  fs->first_data_block = sb.first_data_block;
  fs->inode_count = sb.inodes_count;
  fs->block_count = sb.blocks_count;
  fs->next_alloc_block = sb.first_data_block;
  fs->group_count = ext2_div_round_up(sb.blocks_count - sb.first_data_block, sb.blocks_per_group);
  fs->ext2plus_store_ino = 0;
  fs->ext2plus_base_ino = 0;
  fs->ext2plus_capacity = 0;
  fs->ext2plus_next_index = 0;
  fs->ext2plus_free_head = EXT2PLUS_FREE_NONE;
  fs->ext2plus_free_count = 0;
  fs->ext2plus_chunk_size = 0;
  fs->ext2plus_static_inodes_used = 0;
  cache_reset(fs);
  if (fs->inodes_per_group == 0 || fs->blocks_per_group == 0 || fs->group_count == 0) { return false; }
  if ((sb.feature_incompat & EXT2_INCOMPAT_SPORE_EXT2PLUS) != 0 && !ext2plus_mount_init(fs)) { return false; }
  return true;
}

bool ext2_mount(struct ext2_fs *fs, ext2_read_fn read, void *ctx) {
  return ext2_mount_rw(fs, read, NULL, ctx);
}

bool ext2_info(struct ext2_fs *fs, struct ext2_info *out) {
  struct ext2_super sb;
  if (fs == NULL || out == NULL || !ext2_read_super(fs, &sb)) { return false; }
  uint32_t inode_count = sb.inodes_count;
  uint32_t free_inodes = sb.free_inodes_count;
  if (fs->ext2plus) {
    struct ext2plus_header hdr;
    if (ext2plus_load_header(fs, &hdr)) {
      uint32_t classic_used = sb.inodes_count - sb.free_inodes_count;
      uint32_t static_left = hdr.static_inodes_used < EXT2PLUS_STATIC_INODES_BEFORE_DYNAMIC
                               ? EXT2PLUS_STATIC_INODES_BEFORE_DYNAMIC - hdr.static_inodes_used
                               : 0;
      if (static_left > sb.free_inodes_count) { static_left = sb.free_inodes_count; }
      inode_count = classic_used + static_left + hdr.capacity;
      free_inodes = static_left + (hdr.capacity - hdr.next_index) + hdr.free_count;
    }
  }
  *out = (struct ext2_info){
    .block_size = fs->block_size,
    .block_count = sb.blocks_count,
    .free_blocks = sb.free_blocks_count,
    .inode_count = inode_count,
    .free_inodes = free_inodes,
  };
  return true;
}
