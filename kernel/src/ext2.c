#include "ext2.h"

#include "mem.h"

#include <stddef.h>
#include <stdint.h>

enum {
  EXT2_SUPER_OFFSET = 1024,
  EXT2_SUPER_MAGIC = 0xef53,
  EXT2_ROOT_INO = 2,
  EXT2_S_IFDIR = 0040000,
  EXT2_S_IFREG = 0100000,
  EXT2_FT_REG_FILE = 1,
  EXT2_FT_DIR = 2,
  EXT2_INCOMPAT_FILETYPE = 0x2,
  BLOCK_CACHE_ENTRIES = 32,
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

static uint32_t div_round_up(uint32_t a, uint32_t b) {
  return (a + b - 1) / b;
}

static void cache_reset(struct ext2_fs *fs) {
  for (size_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
    if (block_cache[i].fs == fs) { block_cache[i].valid = false; }
  }
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
    entry = cache_victim();
    if (!fs->read(fs->ctx, (uint64_t)block * fs->block_size, entry->data, fs->block_size)) { return false; }
    entry->valid = true;
    entry->fs = fs;
    entry->block = block;
  }
  entry->age = ++block_cache_clock;
  kmemcpy(dst, entry->data, fs->block_size);
  return true;
}

static bool write_block(struct ext2_fs *fs, uint32_t block, const void *src) {
  if (fs->write == NULL || fs->block_size > sizeof(block_cache[0].data)) { return false; }
  if (!fs->write(fs->ctx, (uint64_t)block * fs->block_size, src, fs->block_size)) { return false; }
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
  out->size = inode.size;
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
  inode.size = node->size;
  inode.links_count = ext2_is_dir(node) ? 2 : 1;
  inode.sectors_count = ((node->size + 511) / 512);
  for (size_t i = 0; i < 15; ++i) {
    inode.block[i] = node->blocks[i];
  }
  return write_bytes(fs, off, &inode, sizeof(inode));
}

static bool file_block(struct ext2_fs *fs, const struct ext2_node *node, uint32_t file_block_index,
                       uint32_t *block_out) {
  if (file_block_index < 12) {
    *block_out = node->blocks[file_block_index];
    return true;
  }
  file_block_index -= 12;
  uint32_t entries_per_block = fs->block_size / sizeof(uint32_t);
  if (file_block_index >= entries_per_block || node->blocks[12] == 0) { return false; }
  uint8_t block[4096];
  if (fs->block_size > sizeof(block) || !read_block(fs, node->blocks[12], block)) { return false; }
  const uint32_t *entries = (const uint32_t *)block;
  *block_out = entries[file_block_index];
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
    for (uint32_t bit = 0; bit < fs->blocks_per_group; ++bit) {
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
  if (file_block_index < 12) {
    if (node->blocks[file_block_index] == 0 && !alloc_block(fs, &node->blocks[file_block_index])) { return false; }
    *block_out = node->blocks[file_block_index];
    return true;
  }
  file_block_index -= 12;
  uint32_t entries_per_block = fs->block_size / sizeof(uint32_t);
  if (file_block_index >= entries_per_block) { return false; }
  if (node->blocks[12] == 0 && !alloc_block(fs, &node->blocks[12])) { return false; }
  uint8_t block[4096];
  if (fs->block_size > sizeof(block) || !read_block(fs, node->blocks[12], block)) { return false; }
  uint32_t *entries = (uint32_t *)block;
  if (entries[file_block_index] == 0) {
    if (!alloc_block(fs, &entries[file_block_index]) || !write_block(fs, node->blocks[12], block)) { return false; }
  }
  *block_out = entries[file_block_index];
  return true;
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
  if (off > node->size) {
    node->size = (uint32_t)off;
    if (!write_inode(fs, node)) { return -1; }
  }
  return (int64_t)done;
}

bool ext2_truncate(struct ext2_fs *fs, struct ext2_node *node, uint64_t size) {
  if (fs->write == NULL) { return false; }
  if (size != 0) {
    node->size = (uint32_t)size;
    return write_inode(fs, node);
  }
  for (size_t i = 0; i < 12; ++i) {
    if (node->blocks[i] != 0) {
      (void)free_block(fs, node->blocks[i]);
      node->blocks[i] = 0;
    }
  }
  if (node->blocks[12] != 0) {
    uint8_t block[4096];
    if (fs->block_size <= sizeof(block) && read_block(fs, node->blocks[12], block)) {
      uint32_t *entries = (uint32_t *)block;
      uint32_t n = fs->block_size / sizeof(uint32_t);
      for (uint32_t i = 0; i < n; ++i) {
        if (entries[i] != 0) { (void)free_block(fs, entries[i]); }
      }
    }
    (void)free_block(fs, node->blocks[12]);
    node->blocks[12] = 0;
  }
  node->size = 0;
  return write_inode(fs, node);
}

bool ext2_dirent(struct ext2_fs *fs, const struct ext2_node *dir, size_t index, struct ext2_dirent *out) {
  if (!ext2_is_dir(dir)) { return false; }
  uint64_t off = 0;
  size_t seen = 0;
  uint8_t header[8];
  while (off + sizeof(header) <= dir->size) {
    uint32_t got = 0;
    if (!ext2_read_file(fs, dir, off, header, sizeof(header), &got) || got != sizeof(header)) { return false; }
    uint32_t ino =
      (uint32_t)header[0] | ((uint32_t)header[1] << 8) | ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
    uint16_t rec_len = (uint16_t)header[4] | ((uint16_t)header[5] << 8);
    uint8_t name_len = header[6];
    uint8_t type = header[7];
    if (rec_len < 8 || off + rec_len > dir->size) { return false; }
    if (ino != 0 && name_len > 0 && name_len <= EXT2_NAME_MAX && name_len + 8 <= rec_len) {
      char name[EXT2_NAME_MAX + 1];
      uint32_t name_got = 0;
      if (!ext2_read_file(fs, dir, off + 8, name, name_len, &name_got) || name_got != name_len) { return false; }
      name[name_len] = '\0';
      bool dot = (name_len == 1 && name[0] == '.') || (name_len == 2 && name[0] == '.' && name[1] == '.');
      if (!dot && seen == index) {
        kmemcpy(out->name, name, name_len + 1);
        out->ino = ino;
        out->type = type;
        return true;
      }
      if (!dot) { ++seen; }
    }
    off += rec_len;
  }
  return false;
}

static bool name_equals(const char *a, size_t a_len, const char *b) {
  if (kstrlen(b) != a_len) { return false; }
  return kmemcmp(a, b, a_len) == 0;
}

static bool lookup_child(struct ext2_fs *fs, const struct ext2_node *dir, const char *name, size_t name_len,
                         struct ext2_node *out) {
  struct ext2_dirent ent;
  for (size_t i = 0; ext2_dirent(fs, dir, i, &ent); ++i) {
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
      uint8_t old_name_len = block[off + 6];
      if (rec_len < 8 || off + rec_len > fs->block_size) { break; }
      uint16_t used = rec_len_for(old_name_len);
      if (rec_len >= used + need) {
        block[off + 4] = (uint8_t)used;
        block[off + 5] = (uint8_t)(used >> 8);
        write_dir_record(block + off + used, ino, (uint16_t)(rec_len - used), name_len, type, name);
        return write_block(fs, disk_block, block);
      }
      off += rec_len;
    }
  }

  uint32_t new_block = 0;
  if (!file_block_for_write(fs, dir, blocks, &new_block) || !read_block(fs, new_block, block)) { return false; }
  kmemset(block, 0, fs->block_size);
  write_dir_record(block, ino, (uint16_t)fs->block_size, name_len, type, name);
  dir->size += fs->block_size;
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
    while (off + 8 <= fs->block_size) {
      uint32_t ino = (uint32_t)block[off] | ((uint32_t)block[off + 1] << 8) | ((uint32_t)block[off + 2] << 16) |
                     ((uint32_t)block[off + 3] << 24);
      uint16_t rec_len = (uint16_t)block[off + 4] | ((uint16_t)block[off + 5] << 8);
      uint8_t name_len = block[off + 6];
      if (rec_len < 8 || off + rec_len > fs->block_size) { break; }
      if (ino != 0 && name_equals((const char *)block + off + 8, name_len, name)) {
        if (ino_out != NULL) { *ino_out = ino; }
        block[off] = block[off + 1] = block[off + 2] = block[off + 3] = 0;
        return write_block(fs, disk_block, block);
      }
      off += rec_len;
    }
  }
  return false;
}

bool ext2_lookup(struct ext2_fs *fs, const char *path, struct ext2_node *out) {
  if (path == NULL || path[0] != '/') { return false; }
  struct ext2_node cur;
  if (!read_inode(fs, EXT2_ROOT_INO, &cur)) { return false; }
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
    if (!lookup_child(fs, &cur, start, len, &cur)) { return false; }
    while (*p == '/') {
      ++p;
    }
  }
  *out = cur;
  return true;
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
  struct ext2_node node = {.ino = ino, .mode = (uint16_t)(dir ? (EXT2_S_IFDIR | 0755) : (EXT2_S_IFREG | 0755))};
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
  struct ext2_node existing;
  if (lookup_child(fs, &new_parent, new_name, kstrlen(new_name), &existing)) { (void)ext2_unlink(fs, new_path); }
  if (!add_dirent(fs, &new_parent, new_name, node.ino, ext2_is_dir(&node) ? EXT2_FT_DIR : EXT2_FT_REG_FILE)) {
    return false;
  }
  return remove_dirent(fs, &old_parent, old_name, NULL);
}
