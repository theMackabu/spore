#include "internal.h"

struct block_path {
  uint32_t root;
  uint32_t depth;
  uint32_t slot[3];
};

static bool locate(const struct ext2_fs *fs, uint32_t index, struct block_path *path) {
  if (index < 12) {
    *path = (struct block_path){.root = index};
    return true;
  }
  uint32_t entries = fs->block_size / sizeof(uint32_t);
  uint32_t span = entries;
  index -= 12;
  for (uint32_t depth = 1; depth <= 3; ++depth) {
    if (index < span) {
      path->root = 11 + depth;
      path->depth = depth;
      for (uint32_t i = depth; i > 0; --i) {
        path->slot[i - 1] = index % entries;
        index /= entries;
      }
      return true;
    }
    index -= span;
    span *= entries;
  }
  return false;
}

static bool read_slot(struct ext2_fs *fs, uint32_t parent, uint32_t slot, uint32_t *block) {
  const uint8_t *entries = ext2_cached_block(fs, parent);
  if (entries == NULL) { return false; }
  kmemcpy(block, entries + slot * sizeof(*block), sizeof(*block));
  return *block < fs->block_count;
}

static bool write_slot(struct ext2_fs *fs, uint32_t parent, uint32_t slot, uint32_t block) {
  return ext2_write_bytes(fs, (uint64_t)parent * fs->block_size + slot * sizeof(block), &block, sizeof(block));
}

static bool walk(struct ext2_fs *fs, const struct ext2_node *node, const struct block_path *path, uint32_t *block,
                 uint32_t *parent, uint32_t *level) {
  *block = node->blocks[path->root];
  *parent = 0;
  *level = 0;
  if (*block >= fs->block_count) { return false; }
  while (*block && *level < path->depth) {
    *parent = *block;
    if (!read_slot(fs, *parent, path->slot[*level], block)) { return false; }
    ++*level;
  }
  return true;
}

bool ext2_file_block(struct ext2_fs *fs, const struct ext2_node *node, uint32_t index, uint32_t *out) {
  struct block_path path;
  uint32_t parent, level;
  return locate(fs, index, &path) && walk(fs, node, &path, out, &parent, &level);
}

int64_t ext2_seek_extent(struct ext2_fs *fs, const struct ext2_node *node, uint64_t off, bool data) {
  if (off >= node->size) { return -6; }
  uint32_t entries = fs->block_size / sizeof(uint32_t);
  while (off < node->size) {
    uint32_t index = off / fs->block_size;
    struct block_path path;
    uint32_t block, parent, level;
    if (!locate(fs, index, &path) || !walk(fs, node, &path, &block, &parent, &level)) { return -EXT2_EIO; }
    if ((block != 0) == data) { return (int64_t)off; }
    uint64_t skip = 1;
    if (level < path.depth) {
      uint64_t within = 0;
      for (uint32_t i = level; i < path.depth; ++i) {
        skip *= entries;
        within = within * entries + path.slot[i];
      }
      skip -= within;
    } else if (parent) {
      const uint8_t *table = ext2_cached_block(fs, parent);
      if (!table) { return -EXT2_EIO; }
      for (uint32_t i = path.slot[level - 1] + 1; i < entries; ++i) {
        kmemcpy(&block, table + i * sizeof(block), sizeof(block));
        if (block >= fs->block_count) { return -EXT2_EIO; }
        if ((block != 0) == data) { break; }
        ++skip;
      }
    }
    off = (index + skip) * fs->block_size;
  }
  return data ? -6 : (int64_t)node->size;
}

int ext2_file_block_for_write(struct ext2_fs *fs, struct ext2_node *node, uint32_t index, uint32_t *out,
                              bool zero_data) {
  struct block_path path;
  uint32_t parent, level;
  if (!locate(fs, index, &path)) { return -EXT2_EFBIG; }
  if (!walk(fs, node, &path, out, &parent, &level)) { return -EXT2_EIO; }
  if (*out) { return 0; }

  uint32_t branch[4];
  uint32_t count = 0;
  int rc = 0;
  for (uint32_t i = level; i <= path.depth; ++i) {
    rc = ext2_alloc_block(fs, &branch[count], i < path.depth || zero_data);
    if (rc) { goto rollback; }
    ++count;
    if (count > 1 && !write_slot(fs, branch[count - 2], path.slot[i - 1], branch[count - 1])) {
      rc = -EXT2_EIO;
      goto rollback;
    }
  }
  if (level == 0) {
    node->blocks[path.root] = branch[0];
  } else if (!write_slot(fs, parent, path.slot[level - 1], branch[0])) {
    rc = -EXT2_EIO;
    goto rollback;
  }
  node->sectors_count += count * (fs->block_size / 512);
  *out = branch[count - 1];
  return 0;

rollback:
  while (count) {
    (void)ext2_free_block(fs, branch[--count]);
  }
  return rc;
}

static bool trim_branch(struct ext2_fs *fs, struct ext2_node *node, uint32_t parent, uint32_t slot, uint32_t block,
                        uint32_t span, uint32_t keep) {
  if (!block || keep >= span) { return true; }
  if (block >= fs->block_count) { return false; }
  if (span > 1) {
    uint32_t entries = fs->block_size / sizeof(uint32_t);
    uint32_t child_span = span / entries;
    bool occupied = false;
    for (uint32_t i = 0; i < entries; ++i) {
      uint32_t child;
      if (!read_slot(fs, block, i, &child)) { return false; }
      if (!child) { continue; }
      uint32_t start = i * child_span;
      uint32_t retain = keep > start ? keep - start : 0;
      if (retain && retain < child_span) {
        if (!trim_branch(fs, node, block, i, child, child_span, retain) || !read_slot(fs, block, i, &child)) {
          return false;
        }
      } else if (!retain) {
        if (!trim_branch(fs, node, block, i, child, child_span, 0)) { return false; }
        child = 0;
      }
      occupied |= child != 0;
    }
    if (occupied) { return true; }
  }
  if (parent) {
    if (!write_slot(fs, parent, slot, 0)) { return false; }
  } else {
    node->blocks[slot] = 0;
    if (!ext2_write_inode(fs, node)) {
      node->blocks[slot] = block;
      return false;
    }
  }
  if (!ext2_free_block(fs, block)) {
    if (parent) {
      (void)write_slot(fs, parent, slot, block);
    } else {
      node->blocks[slot] = block;
    }
    return false;
  }
  node->sectors_count -= fs->block_size / 512;
  return true;
}

bool ext2_trim_blocks(struct ext2_fs *fs, struct ext2_node *node, uint32_t keep) {
  uint32_t span = 1;
  uint32_t start = 0;
  for (uint32_t root = 0; root < 15; ++root) {
    if (root >= 12) { span *= fs->block_size / sizeof(uint32_t); }
    if (!trim_branch(fs, node, 0, root, node->blocks[root], span, keep > start ? keep - start : 0)) { return false; }
    start += span;
  }
  return true;
}
