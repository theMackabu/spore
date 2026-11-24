#include "internal.h"
#include "records.h"

static uint16_t rec_len_for(uint8_t name_len) {
  return (uint16_t)((8u + name_len + 3u) & ~3u);
}

static void write_dir_record(const struct ext2_fs *fs, uint8_t *p, uint32_t ino, uint16_t rec_len, uint8_t name_len, uint8_t type,
                             const char *name) {
  p[0] = (uint8_t)ino;
  p[1] = (uint8_t)(ino >> 8);
  p[2] = (uint8_t)(ino >> 16);
  p[3] = (uint8_t)(ino >> 24);
  p[4] = (uint8_t)rec_len;
  p[5] = (uint8_t)(rec_len >> 8);
  p[6] = name_len;
  p[7] = fs->dirent_filetype ? type : 0;
  kmemcpy(p + 8, name, name_len);
}

static uint8_t dirent_type_for_node(const struct ext2_node *node) {
  if (ext2_is_dir(node)) { return EXT2_FT_DIR; }
  if (ext2_is_symlink(node)) { return EXT2_FT_SYMLINK; }
  return EXT2_FT_REG_FILE;
}

static bool ext2plus_hidden_name(struct ext2_fs *fs, const struct ext2_node *dir, const char *name, size_t name_len) {
  const char hidden[] = ".spore-inodes";
  return fs->ext2plus && dir->ino == EXT2_ROOT_INO && name_len == sizeof(hidden) - 1 &&
         kmemcmp(name, hidden, sizeof(hidden) - 1) == 0;
}

int ext2_visit_dir(struct ext2_fs *fs, const struct ext2_node *dir, uint64_t *cursor, ext2_record_visitor visit,
                     void *ctx) {
  if (!ext2_is_dir(dir)) { return -EXT2_EIO; }
  while (*cursor < dir->size) {
    uint32_t file_block_index = (uint32_t)(*cursor / fs->block_size);
    uint32_t within = (uint32_t)(*cursor % fs->block_size);
    if (fs->block_size - within < 8 || dir->size - *cursor < 8) { return -EXT2_EIO; }
    uint32_t disk_block = 0;
    if (!ext2_file_block(fs, dir, file_block_index, &disk_block) || disk_block == 0) { return -EXT2_EIO; }
    const uint8_t *block = ext2_cached_block(fs, disk_block);
    if (block == NULL) { return -EXT2_EIO; }
    size_t available = fs->block_size - within;
    if (available > dir->size - *cursor) { available = dir->size - *cursor; }
    size_t consumed = 0;
    int rc = spore_ext2_visit_dir_block(block + within, available, fs->ext2plus && dir->ino == EXT2_ROOT_INO,
                                        visit, ctx, &consumed);
    *cursor += consumed;
    if (rc != 0) { return rc < 0 ? -EXT2_EIO : 1; }
  }
  return 0;
}

static uint8_t ext2_dirent_type(uint8_t type) {
  switch (type) {
    case 1: return VFS_DT_REG;
    case 2: return VFS_DT_DIR;
    case 3: return VFS_DT_CHR;
    case 4: return VFS_DT_BLK;
    case 5: return VFS_DT_FIFO;
    case 6: return VFS_DT_SOCK;
    case 7: return VFS_DT_LNK;
    default: return VFS_DT_UNKNOWN;
  }
}

static bool copy_dirent(void *ctx, uint32_t ino, uint8_t type, const char *name, uint8_t len) {
  struct vfs_dirent *out = ctx;
  if (out != NULL) {
    kmemcpy(out->name, name, len);
    out->name[len] = '\0';
    out->ino = ino;
    out->type = ext2_dirent_type(type);
    out->name_len = len;
  }
  return true;
}

int ext2_next_dirent(struct ext2_fs *fs, const struct ext2_node *dir, uint64_t *cursor, struct vfs_dirent *out) {
  ++ext2_stat_counters.dir_iter_count;
  uint64_t start = 0;
  return ext2_visit_dir(fs, dir, cursor != NULL ? cursor : &start, copy_dirent, out);
}

static bool name_equals(const char *a, size_t a_len, const char *b) {
  if (kstrlen(b) != a_len) { return false; }
  return kmemcmp(a, b, a_len) == 0;
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

struct record_location {
  uint64_t block_off;
  uint32_t index;
  struct spore_ext2_dir_match record;
};

static int find_record(struct ext2_fs *fs, const struct ext2_node *dir, const char *name, size_t len,
                       struct record_location *loc) {
  uint32_t blocks = dir->size / fs->block_size;
  if (!ext2_is_dir(dir) || dir->size % fs->block_size) { return -EXT2_EIO; }
  uint32_t hint = ext2_cache_name_block(fs, dir->ino, name, len);
  if (hint >= blocks) { hint = 0; }
  for (uint32_t step = 0; step < blocks; ++step) {
    uint32_t i = step == 0 ? hint : (step <= hint ? step - 1 : step);
    uint32_t block;
    if (!ext2_file_block(fs, dir, i, &block) || !block) { return -EXT2_EIO; }
    const uint8_t *data = ext2_cached_block(fs, block);
    if (!data) { return -EXT2_EIO; }
    int rc = spore_ext2_find_dir_name(data, fs->block_size, (const uint8_t *)name, len, &loc->record);
    if (rc < 0) { return -EXT2_EIO; }
    if (rc) {
      loc->block_off = (uint64_t)block * fs->block_size;
      loc->index = i;
      return 1;
    }
  }
  return 0;
}

static bool update_dir(struct ext2_fs *fs, struct ext2_node *dir) {
  dir->mtime = dir->ctime = ext2_now_sec();
  return ext2_write_inode(fs, dir);
}

static bool add_dirent(struct ext2_fs *fs, struct ext2_node *dir, const char *name, uint32_t ino, uint8_t type) {
  uint8_t len = (uint8_t)kstrlen(name);
  if (!ext2_is_dir(dir) || dir->size % fs->block_size || ext2plus_hidden_name(fs, dir, name, len)) { return false; }
  uint32_t blocks = dir->size / fs->block_size;
  uint8_t record[8 + EXT2_NAME_MAX];
  for (uint32_t step = 0; step < blocks; ++step) {
    uint32_t i = step == 0 ? blocks - 1 : step - 1;
    uint32_t block;
    if (!ext2_file_block(fs, dir, i, &block) || !block) { return false; }
    const uint8_t *data = ext2_cached_block(fs, block);
    if (!data) { return false; }
    struct spore_ext2_dir_slot slot;
    int rc = spore_ext2_find_dir_slot(data, fs->block_size, rec_len_for(len), &slot);
    if (rc < 0) { return false; }
    if (!rc) { continue; }
    uint64_t off = (uint64_t)block * fs->block_size + slot.offset;
    write_dir_record(fs, record, ino, slot.rec_len - slot.used, len, type, name);
    if (!ext2_write_bytes(fs, off + slot.used, record, 8u + len)) { return false; }
    if (slot.used && !ext2_write_bytes(fs, off + 4, &slot.used, sizeof(slot.used))) { return false; }
    if (!update_dir(fs, dir)) {
      if (slot.used) { (void)ext2_write_bytes(fs, off + 4, &slot.rec_len, sizeof(slot.rec_len)); }
      else {
        uint32_t empty = 0;
        (void)ext2_write_bytes(fs, off, &empty, sizeof(empty));
      }
      return false;
    }
    ext2_cache_add_name(fs, dir->ino, name, len, ino, i);
    return true;
  }

  uint32_t block;
  if (dir->size > UINT32_MAX - fs->block_size || ext2_file_block_for_write(fs, dir, blocks, &block, true)) {
    return false;
  }
  write_dir_record(fs, record, ino, fs->block_size, len, type, name);
  dir->size += fs->block_size;
  if (!ext2_write_bytes(fs, (uint64_t)block * fs->block_size, record, 8u + len) || !update_dir(fs, dir)) {
    dir->size -= fs->block_size;
    (void)ext2_trim_blocks(fs, dir, blocks);
    (void)ext2_write_inode(fs, dir);
    return false;
  }
  ext2_cache_add_name(fs, dir->ino, name, len, ino, blocks);
  return true;
}

static bool remove_dirent(struct ext2_fs *fs, struct ext2_node *dir, const char *name) {
  struct record_location loc;
  if (find_record(fs, dir, name, kstrlen(name), &loc) != 1) { return false; }
  uint64_t off = loc.block_off + loc.record.offset;
  uint32_t old = loc.record.ino, replacement = 0;
  size_t bytes = sizeof(uint32_t);
  if (loc.record.prev_offset != UINT32_MAX) {
    off = loc.block_off + loc.record.prev_offset + 4;
    old = loc.record.offset - loc.record.prev_offset;
    replacement = old + loc.record.rec_len;
    bytes = sizeof(uint16_t);
  }
  if (!ext2_write_bytes(fs, off, &replacement, bytes)) { return false; }
  if (!update_dir(fs, dir)) {
    (void)ext2_write_bytes(fs, off, &old, bytes);
    return false;
  }
  ext2_dir_cache_remove_name(fs, dir->ino, name);
  return true;
}

bool ext2_create(struct ext2_fs *fs, const char *path, bool dir, uint16_t mode, struct ext2_node *out) {
  char parent_path[256];
  char name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name))) { return false; }
  struct ext2_node parent;
  if (!ext2_lookup(fs, parent_path, &parent) || !ext2_is_dir(&parent)) { return false; }
  struct ext2_node existing;
  if (ext2_lookup_child(fs, &parent, name, kstrlen(name), &existing)) {
    if (out != NULL) { *out = existing; }
    return true;
  }
  uint32_t ino = 0;
  if (!ext2_alloc_inode(fs, &ino)) { return false; }
  struct ext2_node node = {
    .ino = ino,
    .mode = (uint16_t)((dir ? EXT2_S_IFDIR : EXT2_S_IFREG) | (mode & 07777u)),
    .links_count = (uint16_t)(dir ? 2 : 1),
    .uid = 0,
    .gid = 0,
  };
  uint32_t now = ext2_now_sec();
  node.atime = now;
  node.ctime = now;
  node.mtime = now;
  if (dir) {
    uint32_t block = 0;
    uint8_t buf[4096];
    if (fs->block_size > sizeof(buf) || ext2_file_block_for_write(fs, &node, 0, &block, false) != 0) {
      (void)ext2_free_inode(fs, ino);
      return false;
    }
    node.size = fs->block_size;
    kmemset(buf, 0, fs->block_size);
    write_dir_record(fs, buf, ino, rec_len_for(1), 1, EXT2_FT_DIR, ".");
    write_dir_record(fs, buf + rec_len_for(1), parent.ino, (uint16_t)(fs->block_size - rec_len_for(1)), 2, EXT2_FT_DIR,
                     "..");
    if (!ext2_write_block(fs, block, buf)) {
      node.blocks[0] = 0;
      node.size = 0;
      (void)ext2_free_block(fs, block);
      (void)ext2_free_inode(fs, ino);
      return false;
    }
  }
  if (!ext2_write_inode(fs, &node) || !add_dirent(fs, &parent, name, ino, dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE)) {
    (void)ext2_truncate(fs, &node, 0);
    (void)ext2_free_inode(fs, ino);
    return false;
  }
  if (dir) {
    if (parent.links_count != UINT16_MAX) { ++parent.links_count; }
    parent.mtime = now;
    parent.ctime = now;
    (void)ext2_write_inode(fs, &parent);
    (void)ext2_adjust_group_used_dirs(fs, ino, 1);
  }
  if (out != NULL) { *out = node; }
  return true;
}

bool ext2_link(struct ext2_fs *fs, const char *old_path, const char *new_path) {
  struct ext2_node node;
  return ext2_lookup(fs, old_path, &node) && ext2_link_inode(fs, node.ino, new_path);
}

bool ext2_link_inode(struct ext2_fs *fs, uint32_t ino, const char *new_path) {
  char new_parent_path[256];
  char new_name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(new_path, new_parent_path, sizeof(new_parent_path), new_name, sizeof(new_name))) {
    return false;
  }
  struct ext2_node node;
  struct ext2_node parent;
  struct ext2_node existing;
  if (!ext2_read_inode(fs, ino, &node) || ext2_is_dir(&node) || !ext2_lookup(fs, new_parent_path, &parent) ||
      !ext2_is_dir(&parent) || ext2_lookup_child(fs, &parent, new_name, kstrlen(new_name), &existing)) {
    return false;
  }
  if (node.links_count == UINT16_MAX || (node.links_count == 0 && !ext2_inode_linkable(fs, ino))) { return false; }
  ++node.links_count;
  node.ctime = ext2_now_sec();
  if (!ext2_write_inode(fs, &node)) { return false; }
  if (!add_dirent(fs, &parent, new_name, ino, dirent_type_for_node(&node))) {
    --node.links_count;
    (void)ext2_write_inode(fs, &node);
    return false;
  }
  (void)ext2_set_orphan(fs, ino, false, false);
  return true;
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
      ext2_lookup_child(fs, &parent, name, kstrlen(name), &existing)) {
    return false;
  }
  uint32_t ino = 0;
  if (!ext2_alloc_inode(fs, &ino)) { return false; }
  struct ext2_node node = {
    .ino = ino,
    .mode = (uint16_t)(EXT2_S_IFLNK | 0777),
    .links_count = 1,
    .uid = 0,
    .gid = 0,
  };
  uint32_t now = ext2_now_sec();
  node.atime = now;
  node.ctime = now;
  node.mtime = now;
  size_t target_len = kstrlen(target);
  bool wrote_target = false;
  if (target_len <= sizeof(node.blocks)) {
    kmemcpy(node.blocks, target, target_len);
    node.size = (uint32_t)target_len;
    wrote_target = ext2_write_inode(fs, &node);
  } else {
    int64_t wrote = ext2_write_file(fs, &node, 0, target, target_len);
    wrote_target = wrote >= 0 && (uint64_t)wrote == target_len;
  }
  if (!wrote_target || !add_dirent(fs, &parent, name, ino, EXT2_FT_SYMLINK)) {
    (void)ext2_truncate(fs, &node, 0);
    (void)ext2_free_inode(fs, ino);
    return false;
  }
  return true;
}

int ext2_remove(struct ext2_fs *fs, const char *path) {
  char parent_path[256];
  char name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name))) { return -EXT2_ENOENT; }
  struct ext2_node parent;
  struct ext2_node node;
  if (!ext2_lookup(fs, parent_path, &parent) || !ext2_lookup_child(fs, &parent, name, kstrlen(name), &node)) {
    return -EXT2_ENOENT;
  }
  if (ext2_is_dir(&node)) {
    uint64_t cursor = 0;
    int rc = ext2_next_dirent(fs, &node, &cursor, NULL);
    if (rc < 0) { return rc; }
    if (rc > 0) { return -EXT2_ENOTEMPTY; }
  }
  if (!remove_dirent(fs, &parent, name)) { return -EXT2_EIO; }
  if (ext2_is_dir(&node)) {
    if (parent.links_count > 2) { --parent.links_count; }
    parent.mtime = parent.ctime = ext2_now_sec();
    if (!ext2_write_inode(fs, &parent)) { return -EXT2_EIO; }
    if (node.ino <= fs->inode_count && !ext2_adjust_group_used_dirs(fs, node.ino, -1)) { return -EXT2_EIO; }
    node.links_count = 0;
  } else if (node.links_count) {
    --node.links_count;
  }
  node.ctime = ext2_now_sec();
  if (!ext2_write_inode(fs, &node)) { return -EXT2_EIO; }
  if (node.links_count || ext2_set_orphan(fs, node.ino, true, false)) { return 0; }
  return ext2_truncate(fs, &node, 0) && ext2_free_inode(fs, node.ino) ? 0 : -EXT2_EIO;
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
      !ext2_lookup_child(fs, &old_parent, old_name, kstrlen(old_name), &node)) {
    return false;
  }
  if (name_equals(old_parent_path, kstrlen(old_parent_path), new_parent_path) &&
      name_equals(old_name, kstrlen(old_name), new_name)) {
    return true;
  }
  struct ext2_node existing;
  if (ext2_lookup_child(fs, &new_parent, new_name, kstrlen(new_name), &existing)) {
    if (existing.ino == node.ino) { return true; }
    if ((ext2_is_dir(&node) && !ext2_is_dir(&existing)) || (!ext2_is_dir(&node) && ext2_is_dir(&existing))) {
      return false;
    }
    if (!ext2_unlink(fs, new_path)) { return false; }
    if (!ext2_read_inode(fs, new_parent.ino, &new_parent)) { return false; }
  }
  if (!add_dirent(fs, &new_parent, new_name, node.ino, dirent_type_for_node(&node))) { return false; }
  node.ctime = ext2_now_sec();
  (void)ext2_write_inode(fs, &node);
  if (remove_dirent(fs, &old_parent, old_name)) { return true; }
  (void)ext2_unlink(fs, new_path);
  return false;
}
