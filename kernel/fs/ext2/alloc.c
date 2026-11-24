#include "internal.h"

extern uint32_t ext2_free_bit(const uint8_t *data, size_t bytes, uint32_t start, uint32_t end);

static bool mark_bit(struct ext2_fs *fs, uint32_t bitmap, uint32_t bit, bool allocated) {
  uint64_t offset = (uint64_t)bitmap * fs->block_size + bit / 8;
  uint8_t byte;
  uint8_t mask = (uint8_t)(1u << (bit % 8));
  if (!ext2_read_bytes(fs, offset, &byte, 1) || ((byte & mask) != 0) == allocated) { return false; }
  byte ^= mask;
  return ext2_write_bytes(fs, offset, &byte, 1);
}

static uint32_t choose_bit(const uint8_t *map, size_t bytes, uint32_t start, uint32_t first, uint32_t end) {
  if (start < first || start >= end) { start = first; }
  uint32_t bit = ext2_free_bit(map, bytes, start, end);
  return bit == UINT32_MAX && first < start ? ext2_free_bit(map, bytes, first, start) : bit;
}

int ext2_alloc_block(struct ext2_fs *fs, uint32_t *out, bool zero_block) {
  uint32_t start = fs->next_alloc_block;
  if (start < fs->first_data_block || start >= fs->block_count) { start = fs->first_data_block; }
  uint32_t start_group = (start - fs->first_data_block) / fs->blocks_per_group;
  uint32_t start_bit = (start - fs->first_data_block) % fs->blocks_per_group;
  for (uint32_t group_step = 0; group_step < fs->group_count; ++group_step) {
    uint32_t group = (start_group + group_step) % fs->group_count;
    struct ext2_group_desc gd;
    if (!ext2_read_group_desc(fs, group, &gd)) { return -EXT2_EIO; }
    if (gd.free_blocks_count == 0) { continue; }
    const uint8_t *bitmap = ext2_cached_block(fs, gd.block_bitmap);
    if (bitmap == NULL) { return -EXT2_EIO; }
    uint32_t group_first = fs->first_data_block + group * fs->blocks_per_group;
    uint32_t end = fs->block_count - group_first;
    if (end > fs->blocks_per_group) { end = fs->blocks_per_group; }
    uint32_t bit = choose_bit(bitmap, fs->block_size, group == start_group ? start_bit : 0, group_first == 0, end);
    if (bit == UINT32_MAX) { continue; }
    uint32_t block = group_first + bit;
    if (!mark_bit(fs, gd.block_bitmap, bit, true)) { return -EXT2_EIO; }
    if (gd.free_blocks_count > 0) { --gd.free_blocks_count; }
    if (!ext2_write_group_desc(fs, group, &gd)) { return -EXT2_EIO; }
    struct ext2_super sb;
    if (ext2_read_super(fs, &sb) && sb.free_blocks_count > 0) {
      --sb.free_blocks_count;
      (void)ext2_write_super(fs, &sb);
    }
    if (zero_block) {
      uint8_t zero[4096];
      kmemset(zero, 0, fs->block_size);
      if (!ext2_write_block(fs, block, zero)) { return -EXT2_EIO; }
    }
    *out = block;
    fs->next_alloc_block = block + 1;
    if (fs->next_alloc_block >= fs->block_count) { fs->next_alloc_block = fs->first_data_block; }
    return 0;
  }
  return -EXT2_ENOSPC;
}

bool ext2_free_block(struct ext2_fs *fs, uint32_t block) {
  if (block < fs->first_data_block || block >= fs->block_count) { return false; }
  uint32_t group = (block - fs->first_data_block) / fs->blocks_per_group;
  uint32_t bit = (block - fs->first_data_block) % fs->blocks_per_group;
  if (group >= fs->group_count) { return false; }
  struct ext2_group_desc gd;
  if (!ext2_read_group_desc(fs, group, &gd) || !mark_bit(fs, gd.block_bitmap, bit, false)) { return false; }
  ++gd.free_blocks_count;
  (void)ext2_write_group_desc(fs, group, &gd);
  struct ext2_super sb;
  if (ext2_read_super(fs, &sb)) {
    ++sb.free_blocks_count;
    (void)ext2_write_super(fs, &sb);
  }
  return true;
}

static bool ext2plus_alloc_inode(struct ext2_fs *fs, uint32_t *out) {
  struct ext2plus_header hdr;
  if (!ext2plus_load_header(fs, &hdr)) { return false; }

  uint32_t index = 0;
  if (hdr.free_head != EXT2PLUS_FREE_NONE) {
    index = hdr.free_head;
    uint8_t record[256];
    if (fs->inode_size > sizeof(record) ||
        !ext2plus_read_store(fs, ext2plus_inode_offset(fs, index), record, fs->inode_size)) {
      return false;
    }
    size_t next_off = offsetof(struct ext2_inode_disk, block);
    uint32_t next = (uint32_t)record[next_off] | ((uint32_t)record[next_off + 1] << 8) |
                    ((uint32_t)record[next_off + 2] << 16) | ((uint32_t)record[next_off + 3] << 24);
    if (next != EXT2PLUS_FREE_NONE && next >= hdr.capacity) { return false; }
    hdr.free_head = next;
    if (hdr.free_count > 0) { --hdr.free_count; }
  } else {
    if (hdr.next_index >= hdr.capacity) {
      if (UINT32_MAX - hdr.capacity < hdr.chunk_size) { return false; }
      hdr.capacity += hdr.chunk_size;
      uint64_t store_size = (uint64_t)fs->block_size + (uint64_t)hdr.capacity * fs->inode_size;
      if (store_size > UINT32_MAX) { return false; }
      if (!ext2plus_load_store(fs, NULL) || !ext2_truncate(fs, &fs->ext2plus_store, store_size)) { return false; }
    }
    index = hdr.next_index++;
  }
  if (!ext2plus_write_header(fs, &hdr)) { return false; }
  *out = hdr.base_ino + index;
  return true;
}

static bool alloc_classic_inode(struct ext2_fs *fs, uint32_t *out) {
  uint32_t start = fs->next_alloc_inode;
  if (start < fs->first_ino || start > fs->inode_count) { start = fs->first_ino; }
  uint32_t start_group = (start - 1) / fs->inodes_per_group;
  for (uint32_t step = 0; step < fs->group_count; ++step) {
    uint32_t group = (start_group + step) % fs->group_count;
    uint64_t group_first = (uint64_t)group * fs->inodes_per_group + 1;
    if (group_first > fs->inode_count) { continue; }
    struct ext2_group_desc gd;
    if (!ext2_read_group_desc(fs, group, &gd) || gd.free_inodes_count == 0) { continue; }
    const uint8_t *bitmap = ext2_cached_block(fs, gd.inode_bitmap);
    if (bitmap == NULL) { return false; }
    uint32_t first = fs->first_ino > group_first ? fs->first_ino - (uint32_t)group_first : 0;
    uint32_t end = fs->inode_count - (uint32_t)group_first + 1;
    if (end > fs->inodes_per_group) { end = fs->inodes_per_group; }
    uint32_t bit =
      choose_bit(bitmap, fs->block_size, group == start_group ? (start - 1) % fs->inodes_per_group : first, first, end);
    if (bit == UINT32_MAX) { continue; }
    uint32_t ino = (uint32_t)group_first + bit;
    if (!mark_bit(fs, gd.inode_bitmap, bit, true)) { return false; }
    --gd.free_inodes_count;
    if (!ext2_write_group_desc(fs, group, &gd)) { return false; }
    struct ext2_super sb;
    if (ext2_read_super(fs, &sb) && sb.free_inodes_count > 0) {
      --sb.free_inodes_count;
      (void)ext2_write_super(fs, &sb);
    }
    fs->next_alloc_inode = ino == fs->inode_count ? fs->first_ino : ino + 1;
    *out = ino;
    return true;
  }
  return false;
}

static bool ext2plus_alloc_static_inode(struct ext2_fs *fs, uint32_t *out) {
  struct ext2plus_header hdr;
  if (!ext2plus_load_header(fs, &hdr) || hdr.static_inodes_used >= EXT2PLUS_STATIC_INODES_BEFORE_DYNAMIC) {
    return false;
  }
  if (!alloc_classic_inode(fs, out)) { return false; }
  ++hdr.static_inodes_used;
  if (ext2plus_write_header(fs, &hdr)) { return true; }
  (void)ext2_free_inode(fs, *out);
  *out = 0;
  return false;
}

bool ext2_alloc_inode(struct ext2_fs *fs, uint32_t *out) {
  if (fs->ext2plus) {
    if (ext2plus_alloc_static_inode(fs, out)) { return true; }
    return ext2plus_alloc_inode(fs, out);
  }
  return alloc_classic_inode(fs, out);
}

static bool ext2plus_free_inode(struct ext2_fs *fs, uint32_t ino) {
  if (ino < fs->ext2plus_base_ino) { return false; }
  struct ext2plus_header hdr;
  if (!ext2plus_load_header(fs, &hdr)) { return false; }
  uint32_t index = ino - hdr.base_ino;
  if (index >= hdr.next_index || index >= hdr.capacity) { return false; }

  uint8_t record[256];
  if (fs->inode_size > sizeof(record)) { return false; }
  kmemset(record, 0, fs->inode_size);
  size_t next_off = offsetof(struct ext2_inode_disk, block);
  record[next_off] = (uint8_t)hdr.free_head;
  record[next_off + 1] = (uint8_t)(hdr.free_head >> 8);
  record[next_off + 2] = (uint8_t)(hdr.free_head >> 16);
  record[next_off + 3] = (uint8_t)(hdr.free_head >> 24);
  if (!ext2plus_write_store(fs, ext2plus_inode_offset(fs, index), record, fs->inode_size)) { return false; }

  hdr.free_head = index;
  if (hdr.free_count != UINT32_MAX) { ++hdr.free_count; }
  return ext2plus_write_header(fs, &hdr);
}

static bool free_classic_inode(struct ext2_fs *fs, uint32_t ino) {
  if (ino < fs->first_ino || ino > fs->inode_count) { return false; }
  uint32_t index = ino - 1;
  uint32_t group = index / fs->inodes_per_group;
  uint32_t bit = index % fs->inodes_per_group;
  struct ext2_group_desc gd;
  if (!ext2_read_group_desc(fs, group, &gd)) { return false; }
  const uint8_t *bitmap = ext2_cached_block(fs, gd.inode_bitmap);
  if (bitmap == NULL || !(bitmap[bit / 8] & (1u << (bit % 8)))) { return false; }
  uint8_t zero_inode[256];
  if (fs->inode_size > sizeof(zero_inode)) { return false; }
  kmemset(zero_inode, 0, fs->inode_size);
  uint64_t off = (uint64_t)gd.inode_table * fs->block_size + (uint64_t)bit * fs->inode_size;
  if (!ext2_write_bytes(fs, off, zero_inode, fs->inode_size)) { return false; }
  if (!mark_bit(fs, gd.inode_bitmap, bit, false)) { return false; }
  if (ino < fs->next_alloc_inode) { fs->next_alloc_inode = ino; }
  ++gd.free_inodes_count;
  (void)ext2_write_group_desc(fs, group, &gd);
  struct ext2_super sb;
  if (ext2_read_super(fs, &sb)) {
    ++sb.free_inodes_count;
    (void)ext2_write_super(fs, &sb);
  }
  return true;
}

bool ext2_free_inode(struct ext2_fs *fs, uint32_t ino) {
  ext2_dir_cache_invalidate_inode(fs, ino);
  bool freed = fs->ext2plus && ino >= fs->ext2plus_base_ino ? ext2plus_free_inode(fs, ino) : free_classic_inode(fs, ino);
  if (freed && fs->inode_freed) { fs->inode_freed(ino); }
  return freed;
}

bool ext2_adjust_group_used_dirs(struct ext2_fs *fs, uint32_t ino, int delta) {
  if (ino < fs->first_ino || ino > fs->inode_count) { return false; }
  uint32_t group = (ino - 1) / fs->inodes_per_group;
  struct ext2_group_desc gd;
  if (!ext2_read_group_desc(fs, group, &gd)) { return false; }
  if (delta < 0) {
    uint32_t dec = (uint32_t)(-delta);
    if (gd.used_dirs_count < dec) { return false; }
    gd.used_dirs_count = (uint16_t)(gd.used_dirs_count - dec);
  } else if (delta > 0) {
    uint32_t inc = (uint32_t)delta;
    if ((uint32_t)gd.used_dirs_count + inc > UINT16_MAX) { return false; }
    gd.used_dirs_count = (uint16_t)(gd.used_dirs_count + inc);
  }
  return ext2_write_group_desc(fs, group, &gd);
}
