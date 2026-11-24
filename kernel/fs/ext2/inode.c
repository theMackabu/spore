#include "internal.h"

static bool ext2plus_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out);
static bool ext2plus_write_inode(struct ext2_fs *fs, const struct ext2_node *node);

static void decode_inode(uint32_t ino, const struct ext2_inode_disk *inode, struct ext2_node *out) {
  out->ino = ino;
  out->mode = inode->mode;
  out->links_count = inode->links_count;
  out->uid = inode->uid;
  out->gid = inode->gid;
  out->size = inode->size;
  out->atime = inode->atime;
  out->ctime = inode->ctime;
  out->mtime = inode->mtime;
  out->sectors_count = inode->sectors_count;
  for (size_t i = 0; i < 15; ++i) {
    out->blocks[i] = inode->block[i];
  }
}

static void encode_inode(const struct ext2_node *node, struct ext2_inode_disk *inode) {
  inode->mode = node->mode;
  inode->uid = node->uid;
  inode->gid = node->gid;
  inode->size = node->size;
  inode->atime = node->atime;
  inode->ctime = node->ctime;
  inode->mtime = node->mtime;
  inode->links_count = node->links_count;
  inode->sectors_count = node->sectors_count;
  for (size_t i = 0; i < 15; ++i) {
    inode->block[i] = node->blocks[i];
  }
}

bool ext2_read_raw_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out) {
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
  decode_inode(ino, &inode, out);
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
  decode_inode(ino, &inode, out);
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
  encode_inode(node, &inode);
  bool ok = ext2plus_write_store(fs, ext2plus_inode_offset(fs, index), &inode, sizeof(inode));
  if (ok) { ext2_cache_written_inode(fs, node); }
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
  encode_inode(node, &inode);
  bool ok = ext2_write_bytes(fs, off, &inode, sizeof(inode));
  if (ok) { ext2_cache_written_inode(fs, node); }
  return ok;
}

bool ext2_is_dir(const struct ext2_node *node) {
  return (node->mode & 0170000u) == EXT2_S_IFDIR;
}

bool ext2_is_regular(const struct ext2_node *node) {
  return (node->mode & 0170000u) == EXT2_S_IFREG;
}

bool ext2_is_symlink(const struct ext2_node *node) {
  return (node->mode & 0170000u) == EXT2_S_IFLNK;
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
      while (run_bytes + fs->block_size <= len - done && run_bytes + fs->block_size <= EXT2_DIRECT_READ_MAX) {
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

int64_t ext2_writeback_file(struct ext2_fs *fs, struct ext2_node *node, uint64_t off, const void *src, size_t len) {
  static const uint8_t zeros[4096];
  const uint8_t *data = src;
  if (off > node->size || len > node->size - off) { return -EXT2_EIO; }
  size_t run = 0;
  for (size_t pos = 0; pos < len;) {
    uint64_t at = off + pos;
    size_t within = at % fs->block_size;
    size_t chunk = fs->block_size - within;
    if (chunk > len - pos) { chunk = len - pos; }
    uint32_t block;
    if (!ext2_file_block(fs, node, at / fs->block_size, &block)) { return -EXT2_EIO; }
    const uint8_t *bytes = block ? ext2_peek_block(fs, block) : zeros;
    if (bytes && !kmemcmp(bytes + within, data + pos, chunk)) {
      if (pos > run && ext2_write_file(fs, node, off + run, data + run, pos - run) != (int64_t)(pos - run)) {
        return -EXT2_EIO;
      }
      run = pos + chunk;
    }
    pos += chunk;
  }
  if (len > run && ext2_write_file(fs, node, off + run, data + run, len - run) != (int64_t)(len - run)) {
    return -EXT2_EIO;
  }
  return (int64_t)len;
}

int64_t ext2_write_file(struct ext2_fs *fs, struct ext2_node *node, uint64_t off, const void *src, uint64_t len) {
  const uint8_t *in = src;
  uint64_t done = 0;
  if (fs->write == NULL || fs->block_size > 4096) { return -EXT2_EIO; }
  if (!len) { return 0; }
  if (off > UINT32_MAX || len > UINT32_MAX - off) { return -EXT2_EFBIG; }
  uint32_t old_sectors = node->sectors_count;
  int error = 0;
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
      error = ext2_file_block_for_write(fs, node, file_block_index, &first_disk_block, off < node->size);
      if (error) { break; }
      uint32_t run_blocks = 1;
      while (run_blocks < max_blocks) {
        uint32_t disk_block = 0;
        bool inside_file = off + (uint64_t)run_blocks * fs->block_size < node->size;
        if (ext2_file_block_for_write(fs, node, file_block_index + run_blocks, &disk_block, inside_file)) { break; }
        if (disk_block != first_disk_block + run_blocks) { break; }
        ++run_blocks;
      }
      uint32_t bytes = run_blocks * fs->block_size;
      if (!ext2_write_blocks_direct(fs, first_disk_block, in + done, run_blocks)) {
        error = -EXT2_EIO;
        break;
      }
      done += bytes;
      off += bytes;
      continue;
    }
    uint32_t disk_block = 0;
    error = ext2_file_block_for_write(fs, node, file_block_index, &disk_block, true);
    if (error) { break; }
    uint64_t disk_offset = (uint64_t)disk_block * fs->block_size + within;
    if (!ext2_write_bytes(fs, disk_offset, in + done, chunk)) {
      error = -EXT2_EIO;
      break;
    }
    done += chunk;
    off += chunk;
  }
  if (!done && old_sectors == node->sectors_count) { return error; }
  if (done) {
    if (off > node->size) { node->size = (uint32_t)off; }
    node->mtime = node->ctime = ext2_now_sec();
  }
  if (error && node->sectors_count != old_sectors) {
    uint32_t keep = ((uint64_t)node->size + fs->block_size - 1) / fs->block_size;
    if (!ext2_trim_blocks(fs, node, keep)) { error = -EXT2_EIO; }
  }
  if (!ext2_write_inode(fs, node)) { return -EXT2_EIO; }
  return done ? (int64_t)done : error;
}

bool ext2_truncate(struct ext2_fs *fs, struct ext2_node *node, uint64_t size) {
  if (fs->write == NULL || size > UINT32_MAX) { return false; }
  if (size < node->size || size == 0) {
    if (ext2_is_symlink(node) && node->sectors_count == 0 && node->size <= sizeof(node->blocks)) {
      kmemset((uint8_t *)node->blocks + size, 0, sizeof(node->blocks) - size);
    } else {
      uint32_t tail = size % fs->block_size;
      if (tail != 0) {
        uint32_t block;
        if (!ext2_file_block(fs, node, size / fs->block_size, &block)) { return false; }
        static const uint8_t zero[4096];
        if (block && !ext2_write_bytes(fs, (uint64_t)block * fs->block_size + tail, zero, fs->block_size - tail)) {
          return false;
        }
      }
      if (!ext2_trim_blocks(fs, node, (size + fs->block_size - 1) / fs->block_size)) {
        (void)ext2_write_inode(fs, node);
        return false;
      }
    }
  }
  node->size = (uint32_t)size;
  node->mtime = node->ctime = ext2_now_sec();
  return ext2_write_inode(fs, node);
}

bool ext2_chmod_node(struct ext2_fs *fs, const struct ext2_node *node, uint32_t mode) {
  if (node == NULL) { return false; }
  struct ext2_node copy;
  if (!ext2_read_inode(fs, node->ino, &copy)) { return false; }
  copy.mode = (uint16_t)((copy.mode & 0170000u) | (mode & 07777u));
  copy.ctime = ext2_now_sec();
  return ext2_write_inode(fs, &copy);
}

bool ext2_chown_node(struct ext2_fs *fs, const struct ext2_node *node, uint32_t uid, uint32_t gid) {
  if (node == NULL || uid > UINT16_MAX || gid > UINT16_MAX) { return false; }
  struct ext2_node copy;
  if (!ext2_read_inode(fs, node->ino, &copy)) { return false; }
  copy.uid = (uint16_t)uid;
  copy.gid = (uint16_t)gid;
  copy.ctime = ext2_now_sec();
  return ext2_write_inode(fs, &copy);
}
