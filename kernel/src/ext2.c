#include "ext2.h"

#include "mem.h"

#include <stddef.h>
#include <stdint.h>

enum {
  EXT2_SUPER_OFFSET = 1024,
  EXT2_SUPER_MAGIC = 0xef53,
  EXT2_ROOT_INO = 2,
  EXT2_S_IFDIR = 0040000,
  EXT2_S_IFLNK = 0120000,
  EXT2_S_IFREG = 0100000,
  EXT2_FT_REG_FILE = 1,
  EXT2_FT_DIR = 2,
  EXT2_FT_SYMLINK = 7,
  EXT2_INCOMPAT_FILETYPE = 0x2,
  BLOCK_CACHE_ENTRIES = 32,
  SYMLINK_DEPTH_MAX = 8,
  EXT2_DIRECT_READ_MAX = 128 * 1024,
};

struct ext2_super {
  uint32_t inodes_count;
  uint32_t blocks_count;
  uint32_t reserved_blocks_count;
  uint32_t free_blocks_count;
  uint32_t free_inodes_count;
  uint32_t first_data_block;
  uint32_t log_block_size;
  uint32_t log_frag_size;
  uint32_t blocks_per_group;
  uint32_t frags_per_group;
  uint32_t inodes_per_group;
  uint32_t mtime;
  uint32_t wtime;
  uint16_t mnt_count;
  uint16_t max_mnt_count;
  uint16_t magic;
  uint16_t state;
  uint16_t errors;
  uint16_t minor_rev_level;
  uint32_t lastcheck;
  uint32_t checkinterval;
  uint32_t creator_os;
  uint32_t rev_level;
  uint16_t def_resuid;
  uint16_t def_resgid;
  uint32_t first_ino;
  uint16_t inode_size;
  uint16_t block_group_nr;
  uint32_t feature_compat;
  uint32_t feature_incompat;
  uint32_t feature_ro_compat;
};

struct ext2_group_desc {
  uint32_t block_bitmap;
  uint32_t inode_bitmap;
  uint32_t inode_table;
  uint16_t free_blocks_count;
  uint16_t free_inodes_count;
  uint16_t used_dirs_count;
  uint16_t pad;
  uint8_t reserved[12];
};

struct ext2_inode_disk {
  uint16_t mode;
  uint16_t uid;
  uint32_t size;
  uint32_t atime;
  uint32_t ctime;
  uint32_t mtime;
  uint32_t dtime;
  uint16_t gid;
  uint16_t links_count;
  uint32_t sectors_count;
  uint32_t flags;
  uint32_t osd1;
  uint32_t block[15];
};

struct block_cache_entry {
  bool valid;
  struct ext2_fs *fs;
  uint32_t block;
  uint64_t age;
  uint8_t data[4096];
};

static struct block_cache_entry block_cache[BLOCK_CACHE_ENTRIES];
static uint64_t block_cache_clock;
static uint32_t ext2_now_sec;
static struct ext2_stats ext2_stat_counters;

void ext2_set_now(uint32_t epoch_sec) {
  ext2_now_sec = epoch_sec;
}

static uint32_t now_sec(void) {
  return ext2_now_sec;
}

static uint32_t div_round_up(uint32_t a, uint32_t b) {
  return (a + b - 1) / b;
}

static void cache_reset(struct ext2_fs *fs) {
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (block_cache[i].fs == fs) { block_cache[i].valid = false; }
  }
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

static struct block_cache_entry *cache_victim(void) {
  struct block_cache_entry *victim = &block_cache[0];
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (!block_cache[i].valid) { return &block_cache[i]; }
    if (block_cache[i].age < victim->age) { victim = &block_cache[i]; }
  }
  return victim;
}

static bool read_block(struct ext2_fs *fs, uint32_t block, void *dst) {
  if (fs->block_size > sizeof(block_cache[0].data)) { return false; }
  struct block_cache_entry *entry = cache_find(fs, block);
  if (entry == NULL) {
    ++ext2_stat_counters.block_cache_misses;
    entry = cache_victim();
    if (!fs->read(fs->ctx, (uint64_t)block * fs->block_size, entry->data, fs->block_size)) { return false; }
    entry->valid = true;
    entry->fs = fs;
    entry->block = block;
  } else {
    ++ext2_stat_counters.block_cache_hits;
  }
  entry->age = ++block_cache_clock;
  kmemcpy(dst, entry->data, fs->block_size);
  return true;
}

static bool write_block(struct ext2_fs *fs, uint32_t block, const void *src) {
  if (fs->write == NULL || fs->block_size > sizeof(block_cache[0].data)) { return false; }
  if (!fs->write(fs->ctx, (uint64_t)block * fs->block_size, src, fs->block_size)) { return false; }
  ++ext2_stat_counters.block_cache_writes;
  struct block_cache_entry *entry = cache_find(fs, block);
  if (entry == NULL) {
    entry = cache_victim();
    entry->valid = true;
    entry->fs = fs;
    entry->block = block;
  }
  entry->age = ++block_cache_clock;
  kmemcpy(entry->data, src, fs->block_size);
  return true;
}

static bool read_bytes(struct ext2_fs *fs, uint64_t offset, void *dst, uint32_t len) {
  uint8_t *out = dst;
  uint8_t block[4096];
  while (len > 0) {
    uint32_t block_index = (uint32_t)(offset / fs->block_size);
    uint32_t within = (uint32_t)(offset % fs->block_size);
    uint32_t chunk = fs->block_size - within;
    if (chunk > len) { chunk = len; }
    if (!read_block(fs, block_index, block)) { return false; }
    kmemcpy(out, block + within, chunk);
    out += chunk;
    offset += chunk;
    len -= chunk;
  }
  return true;
}

static bool write_bytes(struct ext2_fs *fs, uint64_t offset, const void *src, uint32_t len) {
  const uint8_t *in = src;
  uint8_t block[4096];
  while (len > 0) {
    uint32_t block_index = (uint32_t)(offset / fs->block_size);
    uint32_t within = (uint32_t)(offset % fs->block_size);
    uint32_t chunk = fs->block_size - within;
    if (chunk > len) { chunk = len; }
    if (within != 0 || chunk != fs->block_size) {
      if (!read_block(fs, block_index, block)) { return false; }
    } else {
      kmemset(block, 0, fs->block_size);
    }
    kmemcpy(block + within, in, chunk);
    if (!write_block(fs, block_index, block)) { return false; }
    in += chunk;
    offset += chunk;
    len -= chunk;
  }
  return true;
}

static bool read_super(struct ext2_fs *fs, struct ext2_super *out) {
  return fs->read(fs->ctx, EXT2_SUPER_OFFSET, out, sizeof(*out));
}

static bool write_super(struct ext2_fs *fs, const struct ext2_super *sb) {
  return fs->write != NULL && write_bytes(fs, EXT2_SUPER_OFFSET, sb, sizeof(*sb));
}

static bool read_group_desc(struct ext2_fs *fs, uint32_t group, struct ext2_group_desc *out) {
  if (group >= fs->group_count) { return false; }
  uint64_t table = fs->block_size == 1024 ? 2ull * fs->block_size : fs->block_size;
  return read_bytes(fs, table + (uint64_t)group * sizeof(*out), out, sizeof(*out));
}

static bool write_group_desc(struct ext2_fs *fs, uint32_t group, const struct ext2_group_desc *in) {
  if (group >= fs->group_count || fs->write == NULL) { return false; }
  uint64_t table = fs->block_size == 1024 ? 2ull * fs->block_size : fs->block_size;
  return write_bytes(fs, table + (uint64_t)group * sizeof(*in), in, sizeof(*in));
}

static bool read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out) {
  if (ino == 0 || ino > fs->inode_count) { return false; }
  uint32_t index = ino - 1;
  uint32_t group = index / fs->inodes_per_group;
  uint32_t local = index % fs->inodes_per_group;
  struct ext2_group_desc gd;
  if (!read_group_desc(fs, group, &gd)) { return false; }
  struct ext2_inode_disk inode;
  uint64_t off = (uint64_t)gd.inode_table * fs->block_size + (uint64_t)local * fs->inode_size;
  if (!read_bytes(fs, off, &inode, sizeof(inode))) { return false; }
  out->ino = ino;
  out->mode = inode.mode;
  out->links_count = inode.links_count;
  out->uid = inode.uid;
  out->gid = inode.gid;
  out->size = inode.size;
  out->atime = inode.atime;
  out->ctime = inode.ctime;
  out->mtime = inode.mtime;
  out->sectors_count = inode.sectors_count;
  for (size_t i = 0; i < 15; ++i) {
    out->blocks[i] = inode.block[i];
  }
  return true;
}

bool ext2_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out) {
  return read_inode(fs, ino, out);
}

static bool write_inode(struct ext2_fs *fs, const struct ext2_node *node) {
  if (fs->write == NULL || node->ino == 0 || node->ino > fs->inode_count) { return false; }
  uint32_t index = node->ino - 1;
  uint32_t group = index / fs->inodes_per_group;
  uint32_t local = index % fs->inodes_per_group;
  struct ext2_group_desc gd;
  if (!read_group_desc(fs, group, &gd)) { return false; }
  struct ext2_inode_disk inode;
  uint64_t off = (uint64_t)gd.inode_table * fs->block_size + (uint64_t)local * fs->inode_size;
  if (!read_bytes(fs, off, &inode, sizeof(inode))) { return false; }
  inode.mode = node->mode;
  inode.uid = node->uid;
  inode.gid = node->gid;
  inode.size = node->size;
  inode.atime = node->atime;
  inode.ctime = node->ctime;
  inode.mtime = node->mtime;
  inode.links_count = node->links_count;
  inode.sectors_count = ext2_is_symlink(node) && node->sectors_count == 0 && node->size <= sizeof(node->blocks)
                          ? 0
                          : ((node->size + 511) / 512);
  for (size_t i = 0; i < 15; ++i) {
    inode.block[i] = node->blocks[i];
  }
  return write_bytes(fs, off, &inode, sizeof(inode));
}

static bool file_block(struct ext2_fs *fs, const struct ext2_node *node, uint32_t file_block_index,
                       uint32_t *block_out) {
  uint32_t entries_per_block = fs->block_size / sizeof(uint32_t);
  uint8_t block[4096];
  if (fs->block_size > sizeof(block) || entries_per_block == 0) { return false; }

  if (file_block_index < 12) {
    *block_out = node->blocks[file_block_index];
    return true;
  }
  file_block_index -= 12;

  if (file_block_index < entries_per_block) {
    if (node->blocks[12] == 0) {
      *block_out = 0;
      return true;
    }
    if (!read_block(fs, node->blocks[12], block)) { return false; }
    const uint32_t *entries = (const uint32_t *)block;
    *block_out = entries[file_block_index];
    return true;
  }
  file_block_index -= entries_per_block;

  uint32_t double_span = entries_per_block * entries_per_block;
  if (file_block_index < double_span) {
    if (node->blocks[13] == 0) {
      *block_out = 0;
      return true;
    }
    if (!read_block(fs, node->blocks[13], block)) { return false; }
    const uint32_t *outer = (const uint32_t *)block;
    uint32_t outer_index = file_block_index / entries_per_block;
    uint32_t inner_index = file_block_index % entries_per_block;
    uint32_t indirect_block = outer[outer_index];
    if (indirect_block == 0) {
      *block_out = 0;
      return true;
    }
    if (!read_block(fs, indirect_block, block)) { return false; }
    const uint32_t *inner = (const uint32_t *)block;
    *block_out = inner[inner_index];
    return true;
  }

  file_block_index -= double_span;
  if (node->blocks[14] == 0) { return false; }
  uint32_t triple_span = entries_per_block * entries_per_block * entries_per_block;
  if (file_block_index >= triple_span) { return false; }
  if (!read_block(fs, node->blocks[14], block)) { return false; }
  const uint32_t *l1 = (const uint32_t *)block;
  uint32_t l1_index = file_block_index / (entries_per_block * entries_per_block);
  uint32_t rem = file_block_index % (entries_per_block * entries_per_block);
  uint32_t l2_block = l1[l1_index];
  if (l2_block == 0) {
    *block_out = 0;
    return true;
  }
  if (!read_block(fs, l2_block, block)) { return false; }
  const uint32_t *l2 = (const uint32_t *)block;
  uint32_t l2_index = rem / entries_per_block;
  uint32_t l3_block = l2[l2_index];
  if (l3_block == 0) {
    *block_out = 0;
    return true;
  }
  if (!read_block(fs, l3_block, block)) { return false; }
  const uint32_t *l3 = (const uint32_t *)block;
  *block_out = l3[rem % entries_per_block];
  return true;
}

bool ext2_mount_rw(struct ext2_fs *fs, ext2_read_fn read, ext2_write_fn write, void *ctx) {
  struct ext2_super sb;
  if (!read(ctx, EXT2_SUPER_OFFSET, &sb, sizeof(sb))) { return false; }
  if (sb.magic != EXT2_SUPER_MAGIC) { return false; }
  if (sb.log_block_size > 2) { return false; }
  if ((sb.feature_incompat & ~EXT2_INCOMPAT_FILETYPE) != 0) { return false; }

  fs->read = read;
  fs->write = write;
  fs->ctx = ctx;
  fs->block_size = 1024u << sb.log_block_size;
  fs->inodes_per_group = sb.inodes_per_group;
  fs->blocks_per_group = sb.blocks_per_group;
  fs->inode_size = sb.inode_size == 0 ? 128 : sb.inode_size;
  fs->first_data_block = sb.first_data_block;
  fs->inode_count = sb.inodes_count;
  fs->block_count = sb.blocks_count;
  fs->group_count = div_round_up(sb.blocks_count - sb.first_data_block, sb.blocks_per_group);
  cache_reset(fs);
  return fs->inodes_per_group != 0 && fs->blocks_per_group != 0 && fs->group_count != 0;
}

bool ext2_mount(struct ext2_fs *fs, ext2_read_fn read, void *ctx) {
  return ext2_mount_rw(fs, read, NULL, ctx);
}

bool ext2_is_dir(const struct ext2_node *node) {
  return (node->mode & EXT2_S_IFDIR) == EXT2_S_IFDIR;
}

bool ext2_is_regular(const struct ext2_node *node) {
  return (node->mode & EXT2_S_IFREG) == EXT2_S_IFREG;
}

bool ext2_is_symlink(const struct ext2_node *node) {
  return (node->mode & EXT2_S_IFLNK) == EXT2_S_IFLNK;
}

bool ext2_info(struct ext2_fs *fs, struct ext2_info *out) {
  struct ext2_super sb;
  if (fs == NULL || out == NULL || !read_super(fs, &sb)) { return false; }
  *out = (struct ext2_info){
    .block_size = fs->block_size,
    .block_count = sb.blocks_count,
    .free_blocks = sb.free_blocks_count,
    .inode_count = sb.inodes_count,
    .free_inodes = sb.free_inodes_count,
  };
  return true;
}

bool ext2_read_file(struct ext2_fs *fs, const struct ext2_node *node, uint64_t off, void *dst, uint32_t len,
                    uint32_t *read_out) {
  uint8_t *out = dst;
  uint32_t done = 0;
  if (off >= node->size) {
    if (read_out != NULL) { *read_out = 0; }
    return true;
  }
  if ((uint64_t)len > (uint64_t)node->size - off) { len = (uint32_t)((uint64_t)node->size - off); }
  uint8_t block[4096];
  if (fs->block_size > sizeof(block)) { return false; }
  while (done < len) {
    uint32_t file_block_index = (uint32_t)(off / fs->block_size);
    uint32_t within = (uint32_t)(off % fs->block_size);
    uint32_t chunk = fs->block_size - within;
    if (chunk > len - done) { chunk = len - done; }
    uint32_t disk_block = 0;
    if (!file_block(fs, node, file_block_index, &disk_block)) { return false; }
    if (within == 0 && chunk == fs->block_size && disk_block != 0) {
      uint32_t run_blocks = 1;
      uint32_t run_bytes = fs->block_size;
      while (done + run_bytes + fs->block_size <= len && run_bytes + fs->block_size <= EXT2_DIRECT_READ_MAX) {
        uint32_t next_block = 0;
        if (!file_block(fs, node, file_block_index + run_blocks, &next_block) ||
            next_block != disk_block + run_blocks) {
          break;
        }
        ++run_blocks;
        run_bytes += fs->block_size;
      }
      if (run_blocks > 1 && fs->read(fs->ctx, (uint64_t)disk_block * fs->block_size, out + done, run_bytes)) {
        done += run_bytes;
        off += run_bytes;
        continue;
      }
    }
    if (disk_block == 0) {
      kmemset(block, 0, fs->block_size);
    } else if (!read_block(fs, disk_block, block)) {
      return false;
    }
    kmemcpy(out + done, block + within, chunk);
    done += chunk;
    off += chunk;
  }
  if (read_out != NULL) { *read_out = done; }
  return true;
}

static uint16_t rec_len_for(uint8_t name_len) {
  return (uint16_t)((8u + name_len + 3u) & ~3u);
}

static void write_dir_record(uint8_t *p, uint32_t ino, uint16_t rec_len, uint8_t name_len, uint8_t type,
                             const char *name) {
  p[0] = (uint8_t)ino;
  p[1] = (uint8_t)(ino >> 8);
  p[2] = (uint8_t)(ino >> 16);
  p[3] = (uint8_t)(ino >> 24);
  p[4] = (uint8_t)rec_len;
  p[5] = (uint8_t)(rec_len >> 8);
  p[6] = name_len;
  p[7] = type;
  kmemcpy(p + 8, name, name_len);
}

static uint8_t dirent_type_for_node(const struct ext2_node *node) {
  if (ext2_is_dir(node)) { return EXT2_FT_DIR; }
  if (ext2_is_symlink(node)) { return EXT2_FT_SYMLINK; }
  return EXT2_FT_REG_FILE;
}

static bool bitmap_set(struct ext2_fs *fs, uint32_t bitmap_block, uint32_t bit, bool used) {
  uint8_t block[4096];
  if (fs->block_size > sizeof(block) || !read_block(fs, bitmap_block, block)) { return false; }
  uint8_t mask = (uint8_t)(1u << (bit % 8u));
  if (used) {
    block[bit / 8u] |= mask;
  } else {
    block[bit / 8u] &= (uint8_t)~mask;
  }
  return write_block(fs, bitmap_block, block);
}

static bool alloc_block(struct ext2_fs *fs, uint32_t *out) {
  uint8_t bitmap[4096];
  if (fs->block_size > sizeof(bitmap)) { return false; }
  for (uint32_t group = 0; group < fs->group_count; ++group) {
    struct ext2_group_desc gd;
    if (!read_group_desc(fs, group, &gd) || gd.free_blocks_count == 0 || !read_block(fs, gd.block_bitmap, bitmap)) {
      continue;
    }
    uint32_t group_first = fs->first_data_block + group * fs->blocks_per_group;
    if (group_first >= fs->block_count) { continue; }
    uint32_t group_blocks = fs->block_count - group_first;
    if (group_blocks > fs->blocks_per_group) { group_blocks = fs->blocks_per_group; }
    for (uint32_t bit = 0; bit < group_blocks; ++bit) {
      if ((bitmap[bit / 8u] & (1u << (bit % 8u))) != 0) { continue; }
      bitmap[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
      if (!write_block(fs, gd.block_bitmap, bitmap)) { return false; }
      --gd.free_blocks_count;
      if (!write_group_desc(fs, group, &gd)) { return false; }
      struct ext2_super sb;
      if (read_super(fs, &sb) && sb.free_blocks_count > 0) {
        --sb.free_blocks_count;
        (void)write_super(fs, &sb);
      }
      uint32_t block = fs->first_data_block + group * fs->blocks_per_group + bit;
      uint8_t zero[4096];
      kmemset(zero, 0, fs->block_size);
      if (!write_block(fs, block, zero)) { return false; }
      *out = block;
      return true;
    }
  }
  return false;
}

static bool free_block(struct ext2_fs *fs, uint32_t block) {
  if (block < fs->first_data_block) { return false; }
  uint32_t rel = block - fs->first_data_block;
  uint32_t group = rel / fs->blocks_per_group;
  uint32_t bit = rel % fs->blocks_per_group;
  struct ext2_group_desc gd;
  if (!read_group_desc(fs, group, &gd) || !bitmap_set(fs, gd.block_bitmap, bit, false)) { return false; }
  ++gd.free_blocks_count;
  (void)write_group_desc(fs, group, &gd);
  struct ext2_super sb;
  if (read_super(fs, &sb)) {
    ++sb.free_blocks_count;
    (void)write_super(fs, &sb);
  }
  return true;
}

static bool alloc_inode(struct ext2_fs *fs, uint32_t *out) {
  uint8_t bitmap[4096];
  if (fs->block_size > sizeof(bitmap)) { return false; }
  for (uint32_t group = 0; group < fs->group_count; ++group) {
    struct ext2_group_desc gd;
    if (!read_group_desc(fs, group, &gd) || gd.free_inodes_count == 0 || !read_block(fs, gd.inode_bitmap, bitmap)) {
      continue;
    }
    for (uint32_t bit = 0; bit < fs->inodes_per_group; ++bit) {
      uint32_t ino = group * fs->inodes_per_group + bit + 1;
      if (ino < 12 || ino > fs->inode_count || (bitmap[bit / 8u] & (1u << (bit % 8u))) != 0) { continue; }
      bitmap[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
      if (!write_block(fs, gd.inode_bitmap, bitmap)) { return false; }
      --gd.free_inodes_count;
      if (!write_group_desc(fs, group, &gd)) { return false; }
      struct ext2_super sb;
      if (read_super(fs, &sb) && sb.free_inodes_count > 0) {
        --sb.free_inodes_count;
        (void)write_super(fs, &sb);
      }
      *out = ino;
      return true;
    }
  }
  return false;
}

static bool free_inode(struct ext2_fs *fs, uint32_t ino) {
  if (ino < 12 || ino > fs->inode_count) { return false; }
  uint32_t index = ino - 1;
  uint32_t group = index / fs->inodes_per_group;
  uint32_t bit = index % fs->inodes_per_group;
  struct ext2_group_desc gd;
  if (!read_group_desc(fs, group, &gd) || !bitmap_set(fs, gd.inode_bitmap, bit, false)) { return false; }
  ++gd.free_inodes_count;
  (void)write_group_desc(fs, group, &gd);
  struct ext2_super sb;
  if (read_super(fs, &sb)) {
    ++sb.free_inodes_count;
    (void)write_super(fs, &sb);
  }
  return true;
}

static bool file_block_for_write(struct ext2_fs *fs, struct ext2_node *node, uint32_t file_block_index,
                                 uint32_t *block_out) {
  uint32_t entries_per_block = fs->block_size / sizeof(uint32_t);
  uint8_t block[4096];
  if (fs->block_size > sizeof(block) || entries_per_block == 0) { return false; }

  if (file_block_index < 12) {
    if (node->blocks[file_block_index] == 0 && !alloc_block(fs, &node->blocks[file_block_index])) { return false; }
    *block_out = node->blocks[file_block_index];
    return true;
  }
  file_block_index -= 12;

  if (file_block_index < entries_per_block) {
    if (node->blocks[12] == 0 && !alloc_block(fs, &node->blocks[12])) { return false; }
    if (!read_block(fs, node->blocks[12], block)) { return false; }
    uint32_t *entries = (uint32_t *)block;
    if (entries[file_block_index] == 0) {
      if (!alloc_block(fs, &entries[file_block_index]) || !write_block(fs, node->blocks[12], block)) { return false; }
    }
    *block_out = entries[file_block_index];
    return true;
  }
  file_block_index -= entries_per_block;

  uint32_t double_span = entries_per_block * entries_per_block;
  if (file_block_index >= double_span) { return false; }
  if (node->blocks[13] == 0 && !alloc_block(fs, &node->blocks[13])) { return false; }
  if (!read_block(fs, node->blocks[13], block)) { return false; }
  uint32_t *outer = (uint32_t *)block;
  uint32_t outer_index = file_block_index / entries_per_block;
  uint32_t inner_index = file_block_index % entries_per_block;
  if (outer[outer_index] == 0) {
    if (!alloc_block(fs, &outer[outer_index]) || !write_block(fs, node->blocks[13], block)) { return false; }
  }
  uint32_t inner_block = outer[outer_index];
  if (!read_block(fs, inner_block, block)) { return false; }
  uint32_t *inner = (uint32_t *)block;
  if (inner[inner_index] == 0) {
    if (!alloc_block(fs, &inner[inner_index]) || !write_block(fs, inner_block, block)) { return false; }
  }
  *block_out = inner[inner_index];
  return true;
}

static void free_indirect_tree(struct ext2_fs *fs, uint32_t block, uint32_t depth) {
  if (block == 0) { return; }
  uint8_t buf[4096];
  if (depth > 0 && fs->block_size <= sizeof(buf) && read_block(fs, block, buf)) {
    uint32_t *entries = (uint32_t *)buf;
    uint32_t n = fs->block_size / sizeof(uint32_t);
    for (uint32_t i = 0; i < n; ++i) {
      if (entries[i] == 0) { continue; }
      if (depth == 1) {
        (void)free_block(fs, entries[i]);
      } else {
        free_indirect_tree(fs, entries[i], depth - 1);
      }
    }
  }
  (void)free_block(fs, block);
}

int64_t ext2_write_file(struct ext2_fs *fs, struct ext2_node *node, uint64_t off, const void *src, uint64_t len) {
  const uint8_t *in = src;
  uint64_t done = 0;
  uint8_t block[4096];
  if (fs->write == NULL || fs->block_size > sizeof(block)) { return -1; }
  while (done < len) {
    uint32_t file_block_index = (uint32_t)(off / fs->block_size);
    uint32_t within = (uint32_t)(off % fs->block_size);
    uint32_t chunk = fs->block_size - within;
    if ((uint64_t)chunk > len - done) { chunk = (uint32_t)(len - done); }
    uint32_t disk_block = 0;
    if (!file_block_for_write(fs, node, file_block_index, &disk_block) || !read_block(fs, disk_block, block)) {
      return -1;
    }
    kmemcpy(block + within, in + done, chunk);
    if (!write_block(fs, disk_block, block)) { return -1; }
    done += chunk;
    off += chunk;
  }
  if (off > node->size) { node->size = (uint32_t)off; }
  uint32_t now = now_sec();
  node->mtime = now;
  node->ctime = now;
  if (!write_inode(fs, node)) { return -1; }
  return (int64_t)done;
}

bool ext2_truncate(struct ext2_fs *fs, struct ext2_node *node, uint64_t size) {
  if (fs->write == NULL) { return false; }
  if (size != 0) {
    node->size = (uint32_t)size;
    uint32_t now = now_sec();
    node->mtime = now;
    node->ctime = now;
    return write_inode(fs, node);
  }
  for (size_t i = 0; i < 12; ++i) {
    if (node->blocks[i] != 0) {
      (void)free_block(fs, node->blocks[i]);
      node->blocks[i] = 0;
    }
  }
  free_indirect_tree(fs, node->blocks[12], 1);
  node->blocks[12] = 0;
  free_indirect_tree(fs, node->blocks[13], 2);
  node->blocks[13] = 0;
  free_indirect_tree(fs, node->blocks[14], 3);
  node->blocks[14] = 0;
  node->size = 0;
  uint32_t now = now_sec();
  node->mtime = now;
  node->ctime = now;
  return write_inode(fs, node);
}

bool ext2_next_dirent(struct ext2_fs *fs, const struct ext2_node *dir, uint64_t *cursor, struct ext2_dirent *out) {
  ++ext2_stat_counters.dir_iter_count;
  if (!ext2_is_dir(dir)) { return false; }
  uint64_t off = cursor == NULL ? 0 : *cursor;
  uint8_t block[4096];
  if (fs->block_size > sizeof(block)) { return false; }
  while (off + 8 <= dir->size) {
    uint32_t file_block_index = (uint32_t)(off / fs->block_size);
    uint32_t within = (uint32_t)(off % fs->block_size);
    if (within + 8 > fs->block_size) {
      off += fs->block_size - within;
      continue;
    }
    uint32_t disk_block = 0;
    if (!file_block(fs, dir, file_block_index, &disk_block)) { return false; }
    if (disk_block == 0) {
      kmemset(block, 0, fs->block_size);
    } else if (!read_block(fs, disk_block, block)) {
      return false;
    }
    uint8_t *record = block + within;
    uint32_t ino =
      (uint32_t)record[0] | ((uint32_t)record[1] << 8) | ((uint32_t)record[2] << 16) | ((uint32_t)record[3] << 24);
    uint16_t rec_len = (uint16_t)record[4] | ((uint16_t)record[5] << 8);
    uint8_t name_len = record[6];
    uint8_t type = record[7];
    if (rec_len < 8 || within + rec_len > fs->block_size || off + rec_len > dir->size) { return false; }
    off += rec_len;
    if (cursor != NULL) { *cursor = off; }
    if (ino != 0 && name_len > 0 && name_len <= EXT2_NAME_MAX && name_len + 8 <= rec_len) {
      char name[EXT2_NAME_MAX + 1];
      kmemcpy(name, record + 8, name_len);
      name[name_len] = '\0';
      bool dot = (name_len == 1 && name[0] == '.') || (name_len == 2 && name[0] == '.' && name[1] == '.');
      if (!dot) {
        kmemcpy(out->name, name, name_len + 1);
        out->ino = ino;
        out->type = type;
        return true;
      }
    }
  }
  return false;
}

bool ext2_dirent(struct ext2_fs *fs, const struct ext2_node *dir, size_t index, struct ext2_dirent *out) {
  uint64_t cursor = 0;
  for (size_t i = 0; ext2_next_dirent(fs, dir, &cursor, out); ++i) {
    if (i == index) { return true; }
  }
  return false;
}

static bool name_equals(const char *a, size_t a_len, const char *b) {
  if (kstrlen(b) != a_len) { return false; }
  return kmemcmp(a, b, a_len) == 0;
}

static bool lookup_child(struct ext2_fs *fs, const struct ext2_node *dir, const char *name, size_t name_len,
                         struct ext2_node *out) {
  ++ext2_stat_counters.lookup_child_count;
  struct ext2_dirent ent;
  uint64_t cursor = 0;
  while (ext2_next_dirent(fs, dir, &cursor, &ent)) {
    if (name_equals(name, name_len, ent.name)) { return read_inode(fs, ent.ino, out); }
  }
  return false;
}

static bool split_parent_path(const char *path, char *parent, size_t parent_cap, char *name, size_t name_cap) {
  if (path == NULL || path[0] != '/') { return false; }
  size_t len = kstrlen(path);
  while (len > 1 && path[len - 1] == '/') {
    --len;
  }
  size_t slash = len;
  while (slash > 0 && path[slash - 1] != '/') {
    --slash;
  }
  size_t name_len = len - slash;
  if (name_len == 0 || name_len >= name_cap) { return false; }
  kmemcpy(name, path + slash, name_len);
  name[name_len] = '\0';
  size_t parent_len = slash <= 1 ? 1 : slash - 1;
  if (parent_len >= parent_cap) { return false; }
  kmemcpy(parent, path, parent_len);
  parent[parent_len] = '\0';
  return true;
}

static bool append_path_component(char *path, size_t cap, const char *name, size_t name_len) {
  size_t len = kstrlen(path);
  if (len == 0) { return false; }
  if (!(len == 1 && path[0] == '/')) {
    if (len + 1 >= cap) { return false; }
    path[len++] = '/';
  }
  if (len + name_len >= cap) { return false; }
  kmemcpy(path + len, name, name_len);
  path[len + name_len] = '\0';
  return true;
}

static bool combine_symlink_path(char *out, size_t cap, const char *parent, const char *target, const char *rest) {
  size_t pos = 0;
  if (cap == 0 || target[0] == '\0') { return false; }
  if (target[0] == '/') {
    for (const char *p = target; *p != '\0'; ++p) {
      if (pos + 1 >= cap) { return false; }
      out[pos++] = *p;
    }
  } else {
    for (const char *p = parent; *p != '\0'; ++p) {
      if (pos + 1 >= cap) { return false; }
      out[pos++] = *p;
    }
    if (!(pos == 1 && out[0] == '/')) {
      if (pos + 1 >= cap) { return false; }
      out[pos++] = '/';
    }
    for (const char *p = target; *p != '\0'; ++p) {
      if (pos + 1 >= cap) { return false; }
      out[pos++] = *p;
    }
  }
  if (rest != NULL && rest[0] != '\0') {
    if (pos == 0 || out[pos - 1] != '/') {
      if (pos + 1 >= cap) { return false; }
      out[pos++] = '/';
    }
    for (const char *p = rest; *p != '\0'; ++p) {
      if (pos + 1 >= cap) { return false; }
      out[pos++] = *p;
    }
  }
  out[pos] = '\0';
  return true;
}

static bool add_dirent(struct ext2_fs *fs, struct ext2_node *dir, const char *name, uint32_t ino, uint8_t type) {
  uint8_t name_len = (uint8_t)kstrlen(name);
  uint16_t need = rec_len_for(name_len);
  uint8_t block[4096];
  if (fs->block_size > sizeof(block)) { return false; }
  uint32_t blocks = (dir->size + fs->block_size - 1) / fs->block_size;
  for (uint32_t bi = 0; bi < blocks; ++bi) {
    uint32_t disk_block = 0;
    if (!file_block(fs, dir, bi, &disk_block) || disk_block == 0 || !read_block(fs, disk_block, block)) { continue; }
    uint32_t off = 0;
    while (off + 8 <= fs->block_size) {
      uint16_t rec_len = (uint16_t)block[off + 4] | ((uint16_t)block[off + 5] << 8);
      uint32_t old_ino = (uint32_t)block[off] | ((uint32_t)block[off + 1] << 8) | ((uint32_t)block[off + 2] << 16) |
                         ((uint32_t)block[off + 3] << 24);
      uint8_t old_name_len = block[off + 6];
      if (rec_len < 8 || off + rec_len > fs->block_size) { break; }
      uint16_t used = old_ino == 0 ? 0 : rec_len_for(old_name_len);
      if (rec_len >= used + need) {
        if (used == 0) {
          write_dir_record(block + off, ino, rec_len, name_len, type, name);
        } else {
          block[off + 4] = (uint8_t)used;
          block[off + 5] = (uint8_t)(used >> 8);
          write_dir_record(block + off + used, ino, (uint16_t)(rec_len - used), name_len, type, name);
        }
        uint32_t now = now_sec();
        dir->mtime = now;
        dir->ctime = now;
        return write_block(fs, disk_block, block) && write_inode(fs, dir);
      }
      off += rec_len;
    }
  }

  uint32_t new_block = 0;
  if (!file_block_for_write(fs, dir, blocks, &new_block) || !read_block(fs, new_block, block)) { return false; }
  kmemset(block, 0, fs->block_size);
  write_dir_record(block, ino, (uint16_t)fs->block_size, name_len, type, name);
  dir->size += fs->block_size;
  uint32_t now = now_sec();
  dir->mtime = now;
  dir->ctime = now;
  return write_block(fs, new_block, block) && write_inode(fs, dir);
}

static bool remove_dirent(struct ext2_fs *fs, struct ext2_node *dir, const char *name, uint32_t *ino_out) {
  uint8_t block[4096];
  if (fs->block_size > sizeof(block)) { return false; }
  uint32_t blocks = (dir->size + fs->block_size - 1) / fs->block_size;
  for (uint32_t bi = 0; bi < blocks; ++bi) {
    uint32_t disk_block = 0;
    if (!file_block(fs, dir, bi, &disk_block) || disk_block == 0 || !read_block(fs, disk_block, block)) { continue; }
    uint32_t off = 0;
    uint32_t prev_off = UINT32_MAX;
    while (off + 8 <= fs->block_size) {
      uint32_t ino = (uint32_t)block[off] | ((uint32_t)block[off + 1] << 8) | ((uint32_t)block[off + 2] << 16) |
                     ((uint32_t)block[off + 3] << 24);
      uint16_t rec_len = (uint16_t)block[off + 4] | ((uint16_t)block[off + 5] << 8);
      uint8_t name_len = block[off + 6];
      if (rec_len < 8 || off + rec_len > fs->block_size) { break; }
      if (ino != 0 && name_equals((const char *)block + off + 8, name_len, name)) {
        if (ino_out != NULL) { *ino_out = ino; }
        if (prev_off != UINT32_MAX) {
          uint16_t prev_rec_len = (uint16_t)block[prev_off + 4] | ((uint16_t)block[prev_off + 5] << 8);
          uint16_t merged = (uint16_t)(prev_rec_len + rec_len);
          block[prev_off + 4] = (uint8_t)merged;
          block[prev_off + 5] = (uint8_t)(merged >> 8);
        } else {
          block[off] = block[off + 1] = block[off + 2] = block[off + 3] = 0;
        }
        uint32_t now = now_sec();
        dir->mtime = now;
        dir->ctime = now;
        return write_block(fs, disk_block, block) && write_inode(fs, dir);
      }
      prev_off = off;
      off += rec_len;
    }
  }
  return false;
}

static bool ext2_readlink_node(struct ext2_fs *fs, const struct ext2_node *node, char *out, size_t cap,
                               size_t *len_out) {
  if (!ext2_is_symlink(node) || cap == 0) { return false; }
  uint32_t len = node->size;
  if ((size_t)len >= cap) { len = (uint32_t)(cap - 1); }
  uint32_t got = 0;
  if (node->sectors_count == 0 && node->size <= sizeof(node->blocks)) {
    kmemcpy(out, node->blocks, len);
    got = len;
  } else if (!ext2_read_file(fs, node, 0, out, len, &got) || got != len) {
    return false;
  }
  out[len] = '\0';
  if (len_out != NULL) { *len_out = len; }
  return true;
}

static bool ext2_lookup_inner(struct ext2_fs *fs, const char *path, struct ext2_node *out, bool follow_final,
                              int depth) {
  if (path == NULL || path[0] != '/' || depth > SYMLINK_DEPTH_MAX) { return false; }
  struct ext2_node cur;
  if (!read_inode(fs, EXT2_ROOT_INO, &cur)) { return false; }
  char parent_path[256] = "/";
  const char *p = path;
  while (*p == '/') {
    ++p;
  }
  if (*p == '\0') {
    *out = cur;
    return true;
  }
  while (*p != '\0') {
    const char *start = p;
    while (*p != '\0' && *p != '/') {
      ++p;
    }
    size_t len = (size_t)(p - start);
    if (len == 0) { return false; }
    struct ext2_node next;
    if (!lookup_child(fs, &cur, start, len, &next)) { return false; }
    while (*p == '/') {
      ++p;
    }
    bool final = *p == '\0';
    if (ext2_is_symlink(&next) && (!final || follow_final)) {
      char target[256];
      char combined[256];
      if (!ext2_readlink_node(fs, &next, target, sizeof(target), NULL) ||
          !combine_symlink_path(combined, sizeof(combined), parent_path, target, p)) {
        return false;
      }
      return ext2_lookup_inner(fs, combined, out, follow_final, depth + 1);
    }
    cur = next;
    if (!append_path_component(parent_path, sizeof(parent_path), start, len)) { return false; }
  }
  *out = cur;
  return true;
}

bool ext2_lookup(struct ext2_fs *fs, const char *path, struct ext2_node *out) {
  ++ext2_stat_counters.lookup_count;
  return ext2_lookup_inner(fs, path, out, true, 0);
}

bool ext2_lstat(struct ext2_fs *fs, const char *path, struct ext2_node *out) {
  ++ext2_stat_counters.lstat_count;
  return ext2_lookup_inner(fs, path, out, false, 0);
}

bool ext2_create(struct ext2_fs *fs, const char *path, bool dir, struct ext2_node *out) {
  char parent_path[256];
  char name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name))) { return false; }
  struct ext2_node parent;
  if (!ext2_lookup(fs, parent_path, &parent) || !ext2_is_dir(&parent)) { return false; }
  struct ext2_node existing;
  if (lookup_child(fs, &parent, name, kstrlen(name), &existing)) {
    if (out != NULL) { *out = existing; }
    return true;
  }
  uint32_t ino = 0;
  if (!alloc_inode(fs, &ino)) { return false; }
  struct ext2_node node = {
    .ino = ino,
    .mode = (uint16_t)(dir ? (EXT2_S_IFDIR | 0755) : (EXT2_S_IFREG | 0755)),
    .links_count = (uint16_t)(dir ? 2 : 1),
    .uid = 0,
    .gid = 0,
  };
  uint32_t now = now_sec();
  node.atime = now;
  node.ctime = now;
  node.mtime = now;
  if (dir) {
    uint32_t block = 0;
    uint8_t buf[4096];
    if (fs->block_size > sizeof(buf) || !alloc_block(fs, &block)) { return false; }
    node.blocks[0] = block;
    node.size = fs->block_size;
    kmemset(buf, 0, fs->block_size);
    write_dir_record(buf, ino, rec_len_for(1), 1, EXT2_FT_DIR, ".");
    write_dir_record(buf + rec_len_for(1), parent.ino, (uint16_t)(fs->block_size - rec_len_for(1)), 2, EXT2_FT_DIR,
                     "..");
    if (!write_block(fs, block, buf)) { return false; }
  }
  if (!write_inode(fs, &node) || !add_dirent(fs, &parent, name, ino, dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE)) {
    return false;
  }
  if (out != NULL) { *out = node; }
  return true;
}

bool ext2_link(struct ext2_fs *fs, const char *old_path, const char *new_path) {
  char new_parent_path[256];
  char new_name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(new_path, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name))) {
    return false;
  }
  struct ext2_node node;
  struct ext2_node parent;
  struct ext2_node existing;
  if (!ext2_lookup(fs, old_path, &node) || ext2_is_dir(&node) || !ext2_lookup(fs, new_parent_path, &parent) ||
      !ext2_is_dir(&parent) || lookup_child(fs, &parent, new_name, kstrlen(new_name), &existing)) {
    return false;
  }
  if (node.links_count == UINT16_MAX || !add_dirent(fs, &parent, new_name, node.ino, dirent_type_for_node(&node))) {
    return false;
  }
  ++node.links_count;
  node.ctime = now_sec();
  return write_inode(fs, &node);
}

bool ext2_symlink(struct ext2_fs *fs, const char *target, const char *link_path) {
  char parent_path[256];
  char name[EXT2_NAME_MAX + 1];
  if (target == NULL || target[0] == '\0' ||
      !split_parent_path(link_path, parent_path, sizeof(parent_path), name, sizeof(name))) {
    return false;
  }
  struct ext2_node parent;
  struct ext2_node existing;
  if (!ext2_lookup(fs, parent_path, &parent) || !ext2_is_dir(&parent) ||
      lookup_child(fs, &parent, name, kstrlen(name), &existing)) {
    return false;
  }
  uint32_t ino = 0;
  if (!alloc_inode(fs, &ino)) { return false; }
  struct ext2_node node = {
    .ino = ino,
    .mode = (uint16_t)(EXT2_S_IFLNK | 0777),
    .links_count = 1,
    .uid = 0,
    .gid = 0,
  };
  uint32_t now = now_sec();
  node.atime = now;
  node.ctime = now;
  node.mtime = now;
  size_t target_len = kstrlen(target);
  bool wrote_target = false;
  if (target_len <= sizeof(node.blocks)) {
    kmemcpy(node.blocks, target, target_len);
    node.size = (uint32_t)target_len;
    wrote_target = write_inode(fs, &node);
  } else {
    int64_t wrote = ext2_write_file(fs, &node, 0, target, target_len);
    wrote_target = wrote >= 0 && (uint64_t)wrote == target_len;
  }
  if (!wrote_target || !add_dirent(fs, &parent, name, ino, EXT2_FT_SYMLINK)) {
    (void)ext2_truncate(fs, &node, 0);
    (void)free_inode(fs, ino);
    return false;
  }
  return true;
}

bool ext2_readlink(struct ext2_fs *fs, const char *path, char *out, size_t cap, size_t *len_out) {
  struct ext2_node node;
  return ext2_lstat(fs, path, &node) && ext2_readlink_node(fs, &node, out, cap, len_out);
}

bool ext2_chmod_node(struct ext2_fs *fs, const struct ext2_node *node, uint32_t mode) {
  if (node == NULL) { return false; }
  struct ext2_node copy = *node;
  copy.mode = (uint16_t)((copy.mode & 0170000u) | (mode & 07777u));
  copy.ctime = now_sec();
  return write_inode(fs, &copy);
}

bool ext2_chmod(struct ext2_fs *fs, const char *path, uint32_t mode) {
  struct ext2_node node;
  return ext2_lookup(fs, path, &node) && ext2_chmod_node(fs, &node, mode);
}

bool ext2_chown_node(struct ext2_fs *fs, const struct ext2_node *node, uint32_t uid, uint32_t gid) {
  if (node == NULL || uid > UINT16_MAX || gid > UINT16_MAX) { return false; }
  struct ext2_node copy = *node;
  copy.uid = (uint16_t)uid;
  copy.gid = (uint16_t)gid;
  copy.ctime = now_sec();
  return write_inode(fs, &copy);
}

bool ext2_chown(struct ext2_fs *fs, const char *path, uint32_t uid, uint32_t gid) {
  struct ext2_node node;
  return ext2_lookup(fs, path, &node) && ext2_chown_node(fs, &node, uid, gid);
}

bool ext2_unlink(struct ext2_fs *fs, const char *path) {
  char parent_path[256];
  char name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name))) { return false; }
  struct ext2_node parent;
  struct ext2_node node;
  if (!ext2_lookup(fs, parent_path, &parent) || !lookup_child(fs, &parent, name, kstrlen(name), &node)) {
    return false;
  }
  if (ext2_is_dir(&node)) {
    struct ext2_dirent ent;
    for (size_t i = 0; ext2_dirent(fs, &node, i, &ent); ++i) {
      if (!name_equals(ent.name, 1, ".") && !name_equals(ent.name, 2, "..")) { return false; }
    }
  }
  if (!remove_dirent(fs, &parent, name, NULL)) { return false; }
  if (node.links_count > 1) {
    --node.links_count;
    node.ctime = now_sec();
    return write_inode(fs, &node);
  }
  (void)ext2_truncate(fs, &node, 0);
  (void)free_inode(fs, node.ino);
  return true;
}

bool ext2_rename(struct ext2_fs *fs, const char *old_path, const char *new_path) {
  char old_parent_path[256];
  char old_name[EXT2_NAME_MAX + 1];
  char new_parent_path[256];
  char new_name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(old_path, old_parent_path, sizeof(old_parent_path), old_name, sizeof(old_name)) ||
      !split_parent_path(new_path, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name))) {
    return false;
  }
  struct ext2_node old_parent;
  struct ext2_node new_parent;
  struct ext2_node node;
  if (!ext2_lookup(fs, old_parent_path, &old_parent) || !ext2_lookup(fs, new_parent_path, &new_parent) ||
      !lookup_child(fs, &old_parent, old_name, kstrlen(old_name), &node)) {
    return false;
  }
  if (name_equals(old_parent_path, kstrlen(old_parent_path), new_parent_path) &&
      name_equals(old_name, kstrlen(old_name), new_name)) {
    return true;
  }
  struct ext2_node existing;
  if (lookup_child(fs, &new_parent, new_name, kstrlen(new_name), &existing)) {
    if ((ext2_is_dir(&node) && !ext2_is_dir(&existing)) || (!ext2_is_dir(&node) && ext2_is_dir(&existing))) {
      return false;
    }
    if (!ext2_unlink(fs, new_path)) { return false; }
    if (!read_inode(fs, new_parent.ino, &new_parent)) { return false; }
  }
  if (!add_dirent(fs, &new_parent, new_name, node.ino, dirent_type_for_node(&node))) {
    return false;
  }
  node.ctime = now_sec();
  (void)write_inode(fs, &node);
  if (remove_dirent(fs, &old_parent, old_name, NULL)) { return true; }
  (void)ext2_unlink(fs, new_path);
  return false;
}
