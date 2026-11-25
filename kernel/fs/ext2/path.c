#include "internal.h"
#include "records.h"

constexpr unsigned SYMLINK_LIMIT = 40;

bool ext2_readlink_node(struct ext2_fs *fs, const struct ext2_node *node, char *out, size_t cap, size_t *len_out) {
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

bool ext2_parent_inode(struct ext2_fs *fs, const struct ext2_node *dir, uint32_t *ino) {
  if (!ext2_is_dir(dir) || !dir->blocks[0]) { return false; }
  const uint8_t *data = ext2_cached_block(fs, dir->blocks[0]);
  struct spore_ext2_dir_match found;
  if (!data || spore_ext2_find_dir_name(data, fs->block_size, (const uint8_t *)"..", 2, &found) != 1) { return false; }
  *ino = found.ino;
  return true;
}

static bool lookup_path(struct ext2_fs *fs, uint32_t root_ino, const char *path, struct ext2_node *out,
                        bool follow_final, struct vfs_entry_name *location) {
  if (!path || path[0] != '/') { return false; }
  struct ext2_node root, current;
  if (!ext2_read_inode(fs, root_ino, &root)) { return false; }
  current = root;
  if (location) { location->parent_ino = 0; }
  char pending[2][256];
  unsigned links = 0;
  for (;;) {
    while (*path == '/') {
      ++path;
    }
    if (!*path) {
      *out = current;
      return true;
    }
    if (!ext2_is_dir(&current)) { return false; }
    const char *name = path;
    while (*path && *path != '/') {
      ++path;
    }
    size_t len = (size_t)(path - name);
    if (len == 1 && *name == '.') { continue; }
    if (len == 2 && name[0] == '.' && name[1] == '.') {
      uint32_t parent;
      if (location) { location->parent_ino = 0; }
      if (current.ino != root_ino &&
          (!ext2_parent_inode(fs, &current, &parent) || !ext2_read_inode(fs, parent, &current))) {
        return false;
      }
      continue;
    }
    struct ext2_node next;
    if (len > EXT2_NAME_MAX || !ext2_lookup_child(fs, &current, name, len, &next)) { return false; }
    if (ext2_is_symlink(&next) && (*path || follow_final)) {
      if (links == SYMLINK_LIMIT || !next.size || next.size >= sizeof(pending[0])) { return false; }
      char *target = pending[links++ % 2];
      size_t suffix = kstrlen(path);
      if (suffix >= sizeof(pending[0]) - next.size ||
          !ext2_readlink_node(fs, &next, target, sizeof(pending[0]), NULL)) {
        return false;
      }
      kmemcpy(target + next.size, path, suffix + 1);
      if (*target == '/') {
        current = root;
        if (location) { location->parent_ino = 0; }
      }
      path = target;
    } else {
      if (*path && !ext2_is_dir(&next)) { return false; }
      if (location) {
        location->parent_ino = current.ino;
        kmemcpy(location->name, name, len);
        location->name[len] = 0;
      }
      current = next;
    }
  }
}

bool ext2_locate_entry_from(struct ext2_fs *fs, uint32_t root_ino, const char *path, uint32_t ino,
                            struct vfs_entry_name *name) {
  struct ext2_node node;
  if (!lookup_path(fs, root_ino, path, &node, true, name) || node.ino != ino) { return false; }
  if (name->parent_ino) { return true; }
  uint32_t parent;
  if (ino == root_ino || !ext2_parent_inode(fs, &node, &parent) || !ext2_read_inode(fs, parent, &node)) {
    return false;
  }
  struct vfs_dirent entry;
  uint64_t cursor = 0;
  while (ext2_next_dirent(fs, &node, &cursor, &entry) > 0) {
    if (entry.ino == ino) {
      name->parent_ino = parent;
      kmemcpy(name->name, entry.name, (size_t)entry.name_len + 1);
      return true;
    }
  }
  return false;
}

const char *ext2_dir_path(struct ext2_fs *fs, uint32_t root, uint32_t ino, char *buf, size_t cap) {
  if (ino == root) { return "/"; }
  if (!cap) { return NULL; }
  char *p = buf + cap;
  *--p = 0;
  while (ino != root) {
    struct ext2_node dir;
    uint32_t parent;
    if (!ext2_inode(fs, ino, &dir) || !dir.links_count || !ext2_parent_inode(fs, &dir, &parent) || parent == ino ||
        !ext2_inode(fs, parent, &dir)) {
      return NULL;
    }
    uint64_t cursor = 0;
    struct vfs_dirent entry;
    int rc;
    do {
      rc = ext2_next_dirent(fs, &dir, &cursor, &entry);
    } while (rc > 0 && entry.ino != ino);
    if (rc <= 0 || (size_t)(p - buf) <= entry.name_len) { return NULL; }
    p -= entry.name_len;
    kmemcpy(p, entry.name, entry.name_len);
    *--p = '/';
    ino = parent;
  }
  return p;
}

bool ext2_lookup_from(struct ext2_fs *fs, uint32_t root_ino, const char *path, struct ext2_node *out) {
  ++ext2_stat_counters.lookup_count;
  return lookup_path(fs, root_ino, path, out, true, NULL);
}

bool ext2_lstat_from(struct ext2_fs *fs, uint32_t root_ino, const char *path, struct ext2_node *out) {
  ++ext2_stat_counters.lstat_count;
  return lookup_path(fs, root_ino, path, out, false, NULL);
}

bool ext2_readlink(struct ext2_fs *fs, const char *path, char *out, size_t cap, size_t *len_out) {
  struct ext2_node node;
  return ext2_lstat(fs, path, &node) && ext2_readlink_node(fs, &node, out, cap, len_out);
}
