#include "internal.h"

static bool ext2plus_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out);
static bool ext2plus_write_inode(struct ext2_fs *fs, const struct ext2_node *node);

bool ext2_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out) {
  if (fs->ext2plus && ino >= fs->ext2plus_base_ino) { return ext2plus_read_inode(fs, ino, out); }
  if (ino == 0 || ino > fs->inode_count) { return false; }
  uint32_t index = ino - 1;
  uint32_t group = index / fs->inodes_per_group;
  uint32_t local = index % fs->inodes_per_group;
  struct ext2_group_desc gd;
  if (!ext2_read_group_desc(fs, group, &gd)) { return false; }
  struct ext2_inode_disk inode;
  uint64_t off = (uint64_t)gd.inode_table * fs->block_size + (uint64_t)local * fs->inode_size;
  if (!ext2_read_bytes(fs, off, &inode, sizeof(inode))) { return false; }
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
  return ext2_read_inode(fs, ino, out);
}

bool ext2plus_load_store(struct ext2_fs *fs, struct ext2_node *out) {
  if (!fs->ext2plus_store_valid || fs->ext2plus_store_ino == 0 || !ext2_is_regular(&fs->ext2plus_store)) {
    return false;
  }
  if (out != NULL) { *out = fs->ext2plus_store; }
  return true;
}

bool ext2plus_read_store(struct ext2_fs *fs, uint64_t off, void *dst, uint32_t len) {
  uint32_t got = 0;
  return ext2plus_load_store(fs, NULL) && off + len >= off && off + len <= fs->ext2plus_store.size &&
         ext2_read_file(fs, &fs->ext2plus_store, off, dst, len, &got) && got == len;
}

bool ext2plus_write_store(struct ext2_fs *fs, uint64_t off, const void *src, uint32_t len) {
  if (!ext2plus_load_store(fs, NULL)) { return false; }
  int64_t wrote = ext2_write_file(fs, &fs->ext2plus_store, off, src, len);
  return wrote == (int64_t)len;
}

uint64_t ext2plus_inode_offset(struct ext2_fs *fs, uint32_t index) {
  return (uint64_t)fs->block_size + (uint64_t)index * fs->inode_size;
}

static bool ext2plus_read_disk_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_inode_disk *inode) {
  if (ino < fs->ext2plus_base_ino) { return false; }
  struct ext2plus_header hdr;
  if (!ext2plus_load_header(fs, &hdr)) { return false; }
  uint32_t index = ino - hdr.base_ino;
  if (index >= hdr.capacity) { return false; }
  uint64_t off = ext2plus_inode_offset(fs, index);
  return ext2plus_read_store(fs, off, inode, sizeof(*inode));
}

static bool ext2plus_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out) {
  struct ext2_inode_disk inode;
  if (!ext2plus_read_disk_inode(fs, ino, &inode) || inode.mode == 0) { return false; }
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

static bool ext2plus_write_inode(struct ext2_fs *fs, const struct ext2_node *node) {
  if (fs->write == NULL || node->ino < fs->ext2plus_base_ino) { return false; }
  struct ext2plus_header hdr;
  if (!ext2plus_load_header(fs, &hdr)) { return false; }
  uint32_t index = node->ino - hdr.base_ino;
  if (index >= hdr.capacity) { return false; }

  struct ext2_inode_disk inode;
  kmemset(&inode, 0, sizeof(inode));
  inode.mode = node->mode;
  inode.uid = node->uid;
  inode.gid = node->gid;
  inode.size = node->size;
  inode.atime = node->atime;
  inode.ctime = node->ctime;
  inode.mtime = node->mtime;
  inode.links_count = node->links_count;
  inode.sectors_count = node->sectors_count;
  for (size_t i = 0; i < 15; ++i) {
    inode.block[i] = node->blocks[i];
  }
  bool ok = ext2plus_write_store(fs, ext2plus_inode_offset(fs, index), &inode, sizeof(inode));
  if (ok) { ext2_dir_cache_invalidate_inode(fs, node->ino); }
  return ok;
}

bool ext2_write_inode(struct ext2_fs *fs, const struct ext2_node *node) {
  if (fs->ext2plus && node->ino >= fs->ext2plus_base_ino) { return ext2plus_write_inode(fs, node); }
  if (fs->write == NULL || node->ino == 0 || node->ino > fs->inode_count) { return false; }
  uint32_t index = node->ino - 1;
  uint32_t group = index / fs->inodes_per_group;
  uint32_t local = index % fs->inodes_per_group;
  struct ext2_group_desc gd;
  if (!ext2_read_group_desc(fs, group, &gd)) { return false; }
  struct ext2_inode_disk inode;
  uint64_t off = (uint64_t)gd.inode_table * fs->block_size + (uint64_t)local * fs->inode_size;
  if (!ext2_read_bytes(fs, off, &inode, sizeof(inode))) { return false; }
  inode.mode = node->mode;
  inode.uid = node->uid;
  inode.gid = node->gid;
  inode.size = node->size;
  inode.atime = node->atime;
  inode.ctime = node->ctime;
  inode.mtime = node->mtime;
  inode.links_count = node->links_count;
  inode.sectors_count = node->sectors_count;
  for (size_t i = 0; i < 15; ++i) {
    inode.block[i] = node->blocks[i];
  }
  bool ok = ext2_write_bytes(fs, off, &inode, sizeof(inode));
  if (ok) { ext2_dir_cache_invalidate_inode(fs, node->ino); }
  return ok;
}

bool ext2_file_block(struct ext2_fs *fs, const struct ext2_node *node, uint32_t index, uint32_t *out) {
  uint32_t entries = fs->block_size / sizeof(uint32_t);
  if (fs->block_size > 4096 || entries == 0) { return false; }
  if (index < 12) {
    *out = node->blocks[index];
    return true;
  }
  index -= 12;
  uint32_t span = entries;
  size_t root = 12;
  while (index >= span) {
    if (root == 14) { return false; }
    index -= span;
    span *= entries;
    ++root;
  }

  uint32_t block = node->blocks[root];
  if (root == 14 && block == 0) { return false; }
  for (size_t depth = root - 11; depth != 0 && block != 0; --depth) {
    span /= entries;
    const uint8_t *pointers = ext2_cached_block(fs, block);
    if (pointers == NULL) { return false; }
    kmemcpy(&block, pointers + (index / span) * sizeof(block), sizeof(block));
    index %= span;
  }
  *out = block;
  return true;
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

bool ext2_read_file(struct ext2_fs *fs, const struct ext2_node *node, uint64_t off, void *dst, uint32_t len,
                    uint32_t *read_out) {
  uint8_t *out = dst;
  uint32_t done = 0;
  if (off >= node->size) {
    if (read_out != NULL) { *read_out = 0; }
    return true;
  }
  if ((uint64_t)len > (uint64_t)node->size - off) { len = (uint32_t)((uint64_t)node->size - off); }
  while (done < len) {
    uint32_t file_block_index = (uint32_t)(off / fs->block_size);
    uint32_t within = (uint32_t)(off % fs->block_size);
    uint32_t chunk = fs->block_size - within;
    if (chunk > len - done) { chunk = len - done; }
    uint32_t disk_block = 0;
    if (!ext2_file_block(fs, node, file_block_index, &disk_block)) { return false; }
    if (within == 0 && chunk == fs->block_size && disk_block != 0) {
      uint32_t run_blocks = 1;
      uint32_t run_bytes = fs->block_size;
      while (done + run_bytes + fs->block_size <= len && run_bytes + fs->block_size <= EXT2_DIRECT_READ_MAX) {
        uint32_t next_block = 0;
        if (!ext2_file_block(fs, node, file_block_index + run_blocks, &next_block) ||
            next_block != disk_block + run_blocks) {
          break;
        }
        ++run_blocks;
        run_bytes += fs->block_size;
      }
      if (run_blocks > 1 && !ext2_cache_range_has_dirty(fs, disk_block, run_blocks) &&
          fs->read(fs->ctx, (uint64_t)disk_block * fs->block_size, out + done, run_bytes)) {
        done += run_bytes;
        off += run_bytes;
        continue;
      }
    }
    if (disk_block == 0) {
      kmemset(out + done, 0, chunk);
    } else {
      const uint8_t *block = ext2_cached_block(fs, disk_block);
      if (block == NULL) { return false; }
      kmemcpy(out + done, block + within, chunk);
    }
    done += chunk;
    off += chunk;
  }
  if (read_out != NULL) { *read_out = done; }
  return true;
}

int64_t ext2_write_file(struct ext2_fs *fs, struct ext2_node *node, uint64_t off, const void *src, uint64_t len) {
  const uint8_t *in = src;
  uint64_t done = 0;
  uint8_t block[4096];
  if (fs->write == NULL || fs->block_size > sizeof(block)) { return -EXT2_EIO; }
  if (off > UINT32_MAX || len > UINT32_MAX || off + len > UINT32_MAX || off + len < off) { return -EXT2_EFBIG; }
  while (done < len) {
    uint32_t file_block_index = (uint32_t)(off / fs->block_size);
    uint32_t within = (uint32_t)(off % fs->block_size);
    uint32_t chunk = fs->block_size - within;
    if ((uint64_t)chunk > len - done) { chunk = (uint32_t)(len - done); }
    if (within == 0 && len - done >= fs->block_size) {
      uint32_t max_blocks = (uint32_t)((len - done) / fs->block_size);
      uint32_t max_direct_blocks = EXT2_DIRECT_WRITE_MAX / fs->block_size;
      if (max_blocks > max_direct_blocks) { max_blocks = max_direct_blocks; }
      uint32_t first_disk_block = 0;
      int rc = ext2_file_block_for_write(fs, node, file_block_index, &first_disk_block, false);
      if (rc != 0) { return rc; }
      uint32_t run_blocks = 1;
      while (run_blocks < max_blocks) {
        uint32_t disk_block = 0;
        rc = ext2_file_block_for_write(fs, node, file_block_index + run_blocks, &disk_block, false);
        if (rc != 0) { break; }
        if (disk_block != first_disk_block + run_blocks) { break; }
        ++run_blocks;
      }
      uint32_t bytes = run_blocks * fs->block_size;
      if (!ext2_write_blocks_direct(fs, first_disk_block, in + done, run_blocks)) { return -EXT2_EIO; }
      done += bytes;
      off += bytes;
      continue;
    }
    uint32_t disk_block = 0;
    int rc = ext2_file_block_for_write(fs, node, file_block_index, &disk_block, true);
    if (rc != 0) { return rc; }
    if (within != 0 || chunk != fs->block_size) {
      if (!ext2_read_block(fs, disk_block, block)) { return -EXT2_EIO; }
    }
    kmemcpy(block + within, in + done, chunk);
    if (!ext2_write_block(fs, disk_block, block)) { return -EXT2_EIO; }
    done += chunk;
    off += chunk;
  }
  if (off > node->size) { node->size = (uint32_t)off; }
  uint32_t now = ext2_now_sec();
  node->mtime = now;
  node->ctime = now;
  if (!ext2_write_inode(fs, node)) { return -EXT2_EIO; }
  return (int64_t)done;
}

bool ext2_truncate(struct ext2_fs *fs, struct ext2_node *node, uint64_t size) {
  if (fs->write == NULL) { return false; }
  if (size != 0) {
    node->size = (uint32_t)size;
    uint32_t now = ext2_now_sec();
    node->mtime = now;
    node->ctime = now;
    return ext2_write_inode(fs, node);
  }
  if (ext2_is_symlink(node) && node->sectors_count == 0 && node->size <= sizeof(node->blocks)) {
    kmemset(node->blocks, 0, sizeof(node->blocks));
    node->size = 0;
    uint32_t now = ext2_now_sec();
    node->mtime = now;
    node->ctime = now;
    return ext2_write_inode(fs, node);
  }
  for (size_t i = 0; i < 12; ++i) {
    if (node->blocks[i] != 0) {
      (void)ext2_free_block(fs, node->blocks[i]);
      node->blocks[i] = 0;
    }
  }
  ext2_free_indirect_tree(fs, node->blocks[12], 1);
  node->blocks[12] = 0;
  ext2_free_indirect_tree(fs, node->blocks[13], 2);
  node->blocks[13] = 0;
  ext2_free_indirect_tree(fs, node->blocks[14], 3);
  node->blocks[14] = 0;
  node->size = 0;
  node->sectors_count = 0;
  uint32_t now = ext2_now_sec();
  node->mtime = now;
  node->ctime = now;
  return ext2_write_inode(fs, node);
}
