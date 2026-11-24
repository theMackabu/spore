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
  switch (node->mode & 0170000u) {
  case 0010000: return 5;
  case 0020000: return 3;
  case EXT2_S_IFDIR: return EXT2_FT_DIR;
  case 0060000: return 4;
  case EXT2_S_IFREG: return EXT2_FT_REG_FILE;
  case EXT2_S_IFLNK: return EXT2_FT_SYMLINK;
  case 0140000: return 6;
  default: return 0;
  }
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
  if (!name_len || name_len >= name_cap || (name_len == 1 && path[slash] == '.') ||
      (name_len == 2 && path[slash] == '.' && path[slash + 1] == '.')) { return false; }
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

static int create_node(struct ext2_fs *fs, const char *path, uint16_t mode, bool exclusive, struct ext2_node *out) {
  if (!fs->write) { return -30; }
  char parent_path[256], name[EXT2_NAME_MAX + 1];
  if (!split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name))) { return -EXT2_EINVAL; }
  struct ext2_node parent, existing;
  if (!ext2_lookup(fs, parent_path, &parent)) { return -EXT2_ENOENT; }
  if (!ext2_is_dir(&parent)) { return -EXT2_ENOTDIR; }
  int found = ext2_find_child(fs, &parent, name, kstrlen(name), &existing);
  if (found < 0) { return found; }
  if (found) {
    if (exclusive) { return -EXT2_EEXIST; }
    if (out) { *out = existing; }
    return 0;
  }
  bool dir = (mode & 0170000u) == EXT2_S_IFDIR;
  if (dir && parent.links_count == UINT16_MAX) { return -EXT2_EMLINK; }
  uint32_t ino;
  if (!ext2_alloc_inode(fs, &ino)) { return -EXT2_ENOSPC; }
  struct ext2_node node = {.ino = ino, .mode = mode, .links_count = dir ? 2 : 1};
  uint32_t now = ext2_now_sec();
  node.atime = now;
  node.ctime = now;
  node.mtime = now;
  if (dir) {
    uint32_t block = 0;
    uint8_t buf[4096];
    int error = ext2_file_block_for_write(fs, &node, 0, &block, false);
    if (error) { (void)ext2_free_inode(fs, ino); return error; }
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
      return -EXT2_EIO;
    }
  }
  bool counted = dir && ino <= fs->inode_count;
  if (!ext2_write_inode(fs, &node)) { goto discard; }
  if (counted && !ext2_adjust_group_used_dirs(fs, ino, 1)) { goto discard; }
  if (dir) { ++parent.links_count; }
  if (!add_dirent(fs, &parent, name, ino, dirent_type_for_node(&node))) {
    if (counted) { (void)ext2_adjust_group_used_dirs(fs, ino, -1); }
    goto discard;
  }
  if (out) { *out = node; }
  return 0;
discard:
  (void)ext2_truncate(fs, &node, 0);
  (void)ext2_free_inode(fs, ino);
  return -EXT2_EIO;
}

int ext2_mknod(struct ext2_fs *fs, const char *path, uint16_t mode, struct ext2_node *out) {
  unsigned type = mode & 0170000u;
  if (type != EXT2_S_IFDIR && type != EXT2_S_IFREG && type != 0010000 && type != 0140000) { return -EXT2_EINVAL; }
  return create_node(fs, path, mode, true, out);
}

bool ext2_create(struct ext2_fs *fs, const char *path, bool dir, uint16_t mode, struct ext2_node *out) {
  return create_node(fs, path, (dir ? EXT2_S_IFDIR : EXT2_S_IFREG) | (mode & 07777u), false, out) == 0;
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
      !ext2_is_dir(&parent) || ext2_find_child(fs, &parent, new_name, kstrlen(new_name), &existing) != 0) {
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
      ext2_find_child(fs, &parent, name, kstrlen(name), &existing) != 0) {
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

static int empty_directory(struct ext2_fs *fs, const struct ext2_node *dir) {
  uint64_t cursor = 0;
  int rc = ext2_next_dirent(fs, dir, &cursor, NULL);
  return rc > 0 ? -EXT2_ENOTEMPTY : rc;
}

static int drop_link(struct ext2_fs *fs, struct ext2_node *node) {
  if (ext2_is_dir(node)) {
    if (node->ino <= fs->inode_count && !ext2_adjust_group_used_dirs(fs, node->ino, -1)) { return -EXT2_EIO; }
    node->links_count = 0;
  } else if (node->links_count) {
    --node->links_count;
  }
  node->ctime = ext2_now_sec();
  if (!ext2_write_inode(fs, node)) { return -EXT2_EIO; }
  if (node->links_count || ext2_set_orphan(fs, node->ino, true, false)) { return 0; }
  return ext2_truncate(fs, node, 0) && ext2_free_inode(fs, node->ino) ? 0 : -EXT2_EIO;
}

int ext2_remove(struct ext2_fs *fs, const char *path) {
  char parent_path[256], name[EXT2_NAME_MAX + 1];
  struct ext2_node parent, node;
  if (!split_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name)) ||
      !ext2_lookup(fs, parent_path, &parent)) { return -EXT2_ENOENT; }
  int found = ext2_find_child(fs, &parent, name, kstrlen(name), &node);
  if (found <= 0) { return found ? found : -EXT2_ENOENT; }
  if (ext2_is_dir(&node)) {
    int rc = empty_directory(fs, &node);
    if (rc) { return rc; }
    if (parent.links_count <= 2) { return -EXT2_EIO; }
    --parent.links_count;
  }
  if (!remove_dirent(fs, &parent, name)) { return -EXT2_EIO; }
  return drop_link(fs, &node);
}

static int check_ancestry(struct ext2_fs *fs, uint32_t source, const struct ext2_node *dest) {
  struct ext2_node current = *dest;
  uint32_t anchor = current.ino;
  uint64_t distance = 0, window = 1;
  while (current.ino != EXT2_ROOT_INO) {
    if (current.ino == source) { return -EXT2_EINVAL; }
    uint32_t parent;
    if (!ext2_parent_inode(fs, &current, &parent) || parent == anchor || !ext2_read_inode(fs, parent, &current)) {
      return -EXT2_EIO;
    }
    if (++distance == window) {
      anchor = current.ino;
      distance = 0;
      window *= 2;
    }
  }
  return 0;
}

static bool replace_record(struct ext2_fs *fs, const struct record_location *loc, uint32_t ino, uint8_t type) {
  uint8_t header[8];
  uint64_t off = loc->block_off + loc->record.offset;
  if (!ext2_read_bytes(fs, off, header, sizeof(header))) { return false; }
  kmemcpy(header, &ino, sizeof(ino));
  header[7] = fs->dirent_filetype ? type : 0;
  return ext2_write_bytes(fs, off, header, sizeof(header));
}

static bool replace_dirent(struct ext2_fs *fs, struct ext2_node *parent, const struct record_location *loc,
                           const char *name, const struct ext2_node *old, const struct ext2_node *next) {
  if (!replace_record(fs, loc, next->ino, dirent_type_for_node(next))) { return false; }
  if (!update_dir(fs, parent)) {
    (void)replace_record(fs, loc, old->ino, dirent_type_for_node(old));
    return false;
  }
  ext2_cache_add_name(fs, parent->ino, name, kstrlen(name), next->ino, loc->index);
  return true;
}

int ext2_renameat(struct ext2_fs *fs, const char *old_path, const char *new_path, bool noreplace) {
  char parent_path[256], old_name[EXT2_NAME_MAX + 1], new_name[EXT2_NAME_MAX + 1];
  struct ext2_node from, to, node, existing;
  if (!split_parent_path(old_path, parent_path, sizeof(parent_path), old_name, sizeof(old_name)) ||
      !ext2_lookup(fs, parent_path, &from)) { return -EXT2_ENOENT; }
  int source = ext2_find_child(fs, &from, old_name, kstrlen(old_name), &node);
  if (source <= 0) { return source ? source : -EXT2_ENOENT; }
  if (!split_parent_path(new_path, parent_path, sizeof(parent_path), new_name, sizeof(new_name)) ||
      !ext2_lookup(fs, parent_path, &to)) { return -EXT2_ENOENT; }
  if (!ext2_is_dir(&to)) { return -EXT2_ENOTDIR; }
  if (ext2plus_hidden_name(fs, &to, new_name, kstrlen(new_name))) { return -EXT2_EEXIST; }
  struct ext2_node *dest = from.ino == to.ino ? &from : &to;
  struct record_location target, dotdot;
  int found = find_record(fs, dest, new_name, kstrlen(new_name), &target);
  if (found < 0) { return found; }
  bool directory = ext2_is_dir(&node);
  if (found) {
    if (noreplace) { return -EXT2_EEXIST; }
    if (target.record.ino == node.ino) { return 0; }
    if (!ext2_read_inode(fs, target.record.ino, &existing)) { return -EXT2_EIO; }
    if (directory != ext2_is_dir(&existing)) { return directory ? -EXT2_ENOTDIR : -EXT2_EISDIR; }
    if (directory) {
      int rc = empty_directory(fs, &existing);
      if (rc) { return rc; }
    }
  }
  bool move_dir = directory && dest != &from;
  if (move_dir) {
    int rc = check_ancestry(fs, node.ino, dest);
    if (rc) { return rc; }
    if (find_record(fs, &node, "..", 2, &dotdot) != 1 || dotdot.record.ino != from.ino || from.links_count <= 2) {
      return -EXT2_EIO;
    }
    if (!found && dest->links_count == UINT16_MAX) { return -EXT2_EMLINK; }
  }
  int delta = (int)move_dir - (int)(directory && found);
  if (delta < 0 && dest->links_count <= 2) { return -EXT2_EIO; }
  dest->links_count += delta;
  if (found) {
    if (!replace_dirent(fs, dest, &target, new_name, &existing, &node)) { return -EXT2_EIO; }
  } else if (!add_dirent(fs, dest, new_name, node.ino, dirent_type_for_node(&node))) {
    return -EXT2_EIO;
  }
  if (move_dir) {
    if (!replace_record(fs, &dotdot, dest->ino, EXT2_FT_DIR)) { goto undo_destination; }
    --from.links_count;
  }
  node.ctime = ext2_now_sec();
  if (ext2_write_inode(fs, &node) && remove_dirent(fs, &from, old_name)) {
    return found ? drop_link(fs, &existing) : 0;
  }
  if (move_dir) {
    (void)replace_record(fs, &dotdot, from.ino, EXT2_FT_DIR);
    ++from.links_count;
  }
undo_destination:
  dest->links_count -= delta;
  if (found) { (void)replace_dirent(fs, dest, &target, new_name, &node, &existing); }
  else { (void)remove_dirent(fs, dest, new_name); }
  return -EXT2_EIO;
}
