#include "internal.h"

static int alloc_block_maybe_zero(struct ext2_fs *fs, uint32_t *out, bool zero_block) {
  uint8_t bitmap[4096];
  if (fs->block_size > sizeof(bitmap)) { return -EXT2_EIO; }
  uint32_t start = fs->next_alloc_block;
  if (start < fs->first_data_block || start >= fs->block_count) { start = fs->first_data_block; }
  uint32_t start_group = (start - fs->first_data_block) / fs->blocks_per_group;
  uint32_t start_bit = (start - fs->first_data_block) % fs->blocks_per_group;
  for (uint32_t group_step = 0; group_step < fs->group_count; ++group_step) {
    uint32_t group = (start_group + group_step) % fs->group_count;
    struct ext2_group_desc gd;
    if (!ext2_read_group_desc(fs, group, &gd)) { return -EXT2_EIO; }
    if (gd.free_blocks_count == 0) { continue; }
    if (!ext2_read_block(fs, gd.block_bitmap, bitmap)) { return -EXT2_EIO; }
    uint32_t group_first = fs->first_data_block + group * fs->blocks_per_group;
    if (group_first >= fs->block_count) { continue; }
    for (uint32_t bit_step = 0; bit_step < fs->blocks_per_group; ++bit_step) {
      uint32_t bit = group == start_group ? (start_bit + bit_step) % fs->blocks_per_group : bit_step;
      uint32_t block = group_first + bit;
      if (block < fs->first_data_block || block >= fs->block_count) { continue; }
      if ((bitmap[bit / 8u] & (1u << (bit % 8u))) != 0) { continue; }
      bitmap[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
      if (!ext2_write_block(fs, gd.block_bitmap, bitmap)) { return -EXT2_EIO; }
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
  }
  return -EXT2_ENOSPC;
}

int ext2_alloc_block(struct ext2_fs *fs, uint32_t *out) {
  return alloc_block_maybe_zero(fs, out, true);
}

bool ext2_free_block(struct ext2_fs *fs, uint32_t block) {
  if (block < fs->first_data_block || block >= fs->block_count) { return false; }
  uint32_t group = (block - fs->first_data_block) / fs->blocks_per_group;
  uint32_t bit = (block - fs->first_data_block) % fs->blocks_per_group;
  if (group >= fs->group_count) { return false; }
  struct ext2_group_desc gd;
  uint8_t bitmap[4096];
  if (fs->block_size > sizeof(bitmap) || !ext2_read_group_desc(fs, group, &gd) || !ext2_read_block(fs, gd.block_bitmap, bitmap)) {
    return false;
  }
  uint8_t mask = (uint8_t)(1u << (bit % 8u));
  if ((bitmap[bit / 8u] & mask) == 0) { return false; }
  bitmap[bit / 8u] &= (uint8_t)~mask;
  if (!ext2_write_block(fs, gd.block_bitmap, bitmap)) { return false; }
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
  uint8_t bitmap[4096];
  if (fs->block_size > sizeof(bitmap)) { return false; }
  for (uint32_t group = 0; group < fs->group_count; ++group) {
    struct ext2_group_desc gd;
    if (!ext2_read_group_desc(fs, group, &gd) || gd.free_inodes_count == 0 || !ext2_read_block(fs, gd.inode_bitmap, bitmap)) {
      continue;
    }
    for (uint32_t bit = 0; bit < fs->inodes_per_group; ++bit) {
      uint32_t ino = group * fs->inodes_per_group + bit + 1;
      if (ino < 12 || ino > fs->inode_count || (bitmap[bit / 8u] & (1u << (bit % 8u))) != 0) { continue; }
      bitmap[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
      if (!ext2_write_block(fs, gd.inode_bitmap, bitmap)) { return false; }
      --gd.free_inodes_count;
      if (!ext2_write_group_desc(fs, group, &gd)) { return false; }
      struct ext2_super sb;
      if (ext2_read_super(fs, &sb) && sb.free_inodes_count > 0) {
        --sb.free_inodes_count;
        (void)ext2_write_super(fs, &sb);
      }
      *out = ino;
      return true;
    }
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

bool ext2_free_inode(struct ext2_fs *fs, uint32_t ino) {
  if (fs->ext2plus && ino >= fs->ext2plus_base_ino) { return ext2plus_free_inode(fs, ino); }
  if (ino < 12 || ino > fs->inode_count) { return false; }
  uint32_t index = ino - 1;
  uint32_t group = index / fs->inodes_per_group;
  uint32_t bit = index % fs->inodes_per_group;
  uint32_t local = index % fs->inodes_per_group;
  struct ext2_group_desc gd;
  uint8_t bitmap[4096];
  if (fs->block_size > sizeof(bitmap) || !ext2_read_group_desc(fs, group, &gd) || !ext2_read_block(fs, gd.inode_bitmap, bitmap)) {
    return false;
  }
  uint8_t mask = (uint8_t)(1u << (bit % 8u));
  if ((bitmap[bit / 8u] & mask) == 0) { return false; }
  uint8_t zero_inode[256];
  if (fs->inode_size > sizeof(zero_inode)) { return false; }
  kmemset(zero_inode, 0, fs->inode_size);
  uint64_t off = (uint64_t)gd.inode_table * fs->block_size + (uint64_t)local * fs->inode_size;
  if (!ext2_write_bytes(fs, off, zero_inode, fs->inode_size)) { return false; }
  bitmap[bit / 8u] &= (uint8_t)~mask;
  if (!ext2_write_block(fs, gd.inode_bitmap, bitmap)) { return false; }
  ++gd.free_inodes_count;
  (void)ext2_write_group_desc(fs, group, &gd);
  struct ext2_super sb;
  if (ext2_read_super(fs, &sb)) {
    ++sb.free_inodes_count;
    (void)ext2_write_super(fs, &sb);
  }
  return true;
}

bool ext2_adjust_group_used_dirs(struct ext2_fs *fs, uint32_t ino, int delta) {
  if (ino < 12 || ino > fs->inode_count) { return false; }
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

int ext2_file_block_for_write(struct ext2_fs *fs, struct ext2_node *node, uint32_t file_block_index,
                                uint32_t *block_out, bool zero_data_block) {
  uint32_t entries_per_block = fs->block_size / sizeof(uint32_t);
  uint8_t block[4096];
  uint8_t block2[4096];
  if (fs->block_size > sizeof(block) || entries_per_block == 0) { return -EXT2_EIO; }

  if (file_block_index < 12) {
    if (node->blocks[file_block_index] == 0) {
      int rc = alloc_block_maybe_zero(fs, &node->blocks[file_block_index], zero_data_block);
      if (rc != 0) { return rc; }
    }
    *block_out = node->blocks[file_block_index];
    return 0;
  }
  file_block_index -= 12;

  if (file_block_index < entries_per_block) {
    if (node->blocks[12] == 0) {
      int rc = ext2_alloc_block(fs, &node->blocks[12]);
      if (rc != 0) { return rc; }
    }
    if (!ext2_read_block(fs, node->blocks[12], block)) { return -EXT2_EIO; }
    uint32_t *entries = (uint32_t *)block;
    if (entries[file_block_index] == 0) {
      int rc = alloc_block_maybe_zero(fs, &entries[file_block_index], zero_data_block);
      if (rc != 0) { return rc; }
      if (!ext2_write_block(fs, node->blocks[12], block)) { return -EXT2_EIO; }
    }
    *block_out = entries[file_block_index];
    return 0;
  }
  file_block_index -= entries_per_block;

  uint64_t double_span = (uint64_t)entries_per_block * entries_per_block;
  if ((uint64_t)file_block_index < double_span) {
    if (node->blocks[13] == 0) {
      int rc = ext2_alloc_block(fs, &node->blocks[13]);
      if (rc != 0) { return rc; }
    }
    if (!ext2_read_block(fs, node->blocks[13], block)) { return -EXT2_EIO; }
    uint32_t *outer = (uint32_t *)block;
    uint32_t outer_index = file_block_index / entries_per_block;
    uint32_t inner_index = file_block_index % entries_per_block;
    if (outer[outer_index] == 0) {
      int rc = ext2_alloc_block(fs, &outer[outer_index]);
      if (rc != 0) { return rc; }
      if (!ext2_write_block(fs, node->blocks[13], block)) { return -EXT2_EIO; }
    }
    uint32_t inner_block = outer[outer_index];
    if (!ext2_read_block(fs, inner_block, block)) { return -EXT2_EIO; }
    uint32_t *inner = (uint32_t *)block;
    if (inner[inner_index] == 0) {
      int rc = alloc_block_maybe_zero(fs, &inner[inner_index], zero_data_block);
      if (rc != 0) { return rc; }
      if (!ext2_write_block(fs, inner_block, block)) { return -EXT2_EIO; }
    }
    *block_out = inner[inner_index];
    return 0;
  }
  file_block_index -= (uint32_t)double_span;

  uint64_t triple_span = double_span * entries_per_block;
  if ((uint64_t)file_block_index >= triple_span) { return -EXT2_EFBIG; }
  if (node->blocks[14] == 0) {
    int rc = ext2_alloc_block(fs, &node->blocks[14]);
    if (rc != 0) { return rc; }
  }
  if (!ext2_read_block(fs, node->blocks[14], block)) { return -EXT2_EIO; }
  uint32_t *outer = (uint32_t *)block;
  uint32_t outer_index = (uint32_t)((uint64_t)file_block_index / double_span);
  uint32_t rem = (uint32_t)((uint64_t)file_block_index % double_span);
  if (outer[outer_index] == 0) {
    int rc = ext2_alloc_block(fs, &outer[outer_index]);
    if (rc != 0) { return rc; }
    if (!ext2_write_block(fs, node->blocks[14], block)) { return -EXT2_EIO; }
  }
  uint32_t middle_block = outer[outer_index];
  if (!ext2_read_block(fs, middle_block, block2)) { return -EXT2_EIO; }
  uint32_t *middle = (uint32_t *)block2;
  uint32_t middle_index = rem / entries_per_block;
  uint32_t inner_index = rem % entries_per_block;
  if (middle[middle_index] == 0) {
    int rc = ext2_alloc_block(fs, &middle[middle_index]);
    if (rc != 0) { return rc; }
    if (!ext2_write_block(fs, middle_block, block2)) { return -EXT2_EIO; }
  }
  uint32_t inner_block = middle[middle_index];
  if (!ext2_read_block(fs, inner_block, block)) { return -EXT2_EIO; }
  uint32_t *inner = (uint32_t *)block;
  if (inner[inner_index] == 0) {
    int rc = alloc_block_maybe_zero(fs, &inner[inner_index], zero_data_block);
    if (rc != 0) { return rc; }
    if (!ext2_write_block(fs, inner_block, block)) { return -EXT2_EIO; }
  }
  *block_out = inner[inner_index];
  return 0;
}

void ext2_free_indirect_tree(struct ext2_fs *fs, uint32_t block, uint32_t depth) {
  if (block == 0) { return; }
  uint8_t buf[4096];
  if (depth > 0 && fs->block_size <= sizeof(buf) && ext2_read_block(fs, block, buf)) {
    uint32_t *entries = (uint32_t *)buf;
    uint32_t n = fs->block_size / sizeof(uint32_t);
    for (uint32_t i = 0; i < n; ++i) {
      if (entries[i] == 0) { continue; }
      if (depth == 1) {
        (void)ext2_free_block(fs, entries[i]);
      } else {
        ext2_free_indirect_tree(fs, entries[i], depth - 1);
      }
    }
  }
  (void)ext2_free_block(fs, block);
}
