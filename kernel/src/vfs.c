#include "vfs.h"

#include "mem.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

static struct ramfs *root_ramfs;
static struct ext2_fs *root_ext2;
static uint64_t root_hhdm;
static uint8_t *exec_buf;
static uint64_t exec_buf_cap;

static bool starts_with(const char *s, const char *prefix) {
  while (*prefix != '\0') {
    if (*s++ != *prefix++) { return false; }
  }
  return true;
}

static void from_ramfs(const struct ramfs_node *node, struct vfs_node *out) {
  *out = (struct vfs_node){
    .backend = VFS_RAMFS,
    .ino = node->ino,
    .is_dir = node->is_dir,
    .writable = true,
    .device = node->device,
    .size = node->size,
    .ramfs = *node,
  };
}

static void from_ext2(const struct ext2_node *node, struct vfs_node *out) {
  *out = (struct vfs_node){
    .backend = VFS_EXT2,
    .ino = node->ino,
    .is_dir = ext2_is_dir(node),
    .writable = false,
    .device = RAMFS_DEV_NONE,
    .size = node->size,
    .ext2 = *node,
  };
}

void vfs_init(struct ramfs *ramfs, struct ext2_fs *ext2, uint64_t hhdm_offset) {
  root_ramfs = ramfs;
  root_ext2 = ext2;
  root_hhdm = hhdm_offset;
}

static bool ensure_exec_buf(uint64_t need) {
  if (exec_buf != NULL && need <= exec_buf_cap) { return true; }
  if (need > 4ull * 1024 * 1024) { return false; }
  uint64_t pages = (4ull * 1024 * 1024) / PAGE_SIZE;
  uint64_t first = 0;
  for (uint64_t i = 0; i < pages; ++i) {
    uint64_t pa = pmm_alloc_zero_page();
    if (pa == 0) { return false; }
    if (i == 0) {
      first = pa;
    } else if (pa != first + i * PAGE_SIZE) {
      return false;
    }
  }
  exec_buf = (uint8_t *)(uintptr_t)(root_hhdm + first);
  exec_buf_cap = pages * PAGE_SIZE;
  return true;
}

static bool lookup_ramfs(const char *path, struct vfs_node *out) {
  struct ramfs_node node;
  if (root_ramfs == NULL || !ramfs_lookup_node(root_ramfs, path, &node)) { return false; }
  from_ramfs(&node, out);
  return true;
}

bool vfs_lookup(const char *path, struct vfs_node *out) {
  if (starts_with(path, "/dev")) {
    if (lookup_ramfs(path, out)) { return true; }
  }
  if (root_ext2 != NULL) {
    struct ext2_node node;
    if (ext2_lookup(root_ext2, path, &node)) {
      from_ext2(&node, out);
      return true;
    }
  }
  return lookup_ramfs(path, out);
}

bool vfs_lookup_exec(const char *path, const void **data, uint64_t *size) {
  if (root_ext2 != NULL) {
    struct ext2_node node;
    if (ext2_lookup(root_ext2, path, &node) && ext2_is_regular(&node)) {
      if (!ensure_exec_buf(node.size)) { return false; }
      uint32_t got = 0;
      if (!ext2_read_file(root_ext2, &node, 0, exec_buf, node.size, &got) || got != node.size) { return false; }
      *data = exec_buf;
      *size = node.size;
      return true;
    }
  }

  struct ramfs_file file;
  if (root_ramfs != NULL && ramfs_lookup(root_ramfs, path, &file)) {
    *data = file.data;
    *size = file.size;
    return true;
  }
  return false;
}

bool vfs_mkdir(const char *path) {
  if (root_ext2 != NULL && !starts_with(path, "/dev")) {
    struct ext2_node node;
    return ext2_create(root_ext2, path, true, &node);
  }
  return root_ramfs != NULL && ramfs_mkdir(root_ramfs, path);
}

bool vfs_create(const char *path, struct vfs_node *out) {
  if (root_ext2 != NULL && !starts_with(path, "/dev")) {
    struct ext2_node node;
    if (!ext2_create(root_ext2, path, false, &node)) { return false; }
    from_ext2(&node, out);
    out->writable = true;
    return true;
  }
  struct ramfs_node node;
  if (root_ramfs == NULL || !ramfs_create(root_ramfs, path, &node)) { return false; }
  from_ramfs(&node, out);
  return true;
}

bool vfs_truncate(const struct vfs_node *node, uint64_t size) {
  if (node->backend == VFS_EXT2) {
    struct ext2_node ext = node->ext2;
    return ext2_truncate(root_ext2, &ext, size);
  }
  if (node->backend != VFS_RAMFS) { return false; }
  return ramfs_truncate(node->ramfs.fs, node->ramfs.index, size);
}

bool vfs_unlink(const char *path) {
  if (root_ext2 != NULL && !starts_with(path, "/dev")) { return ext2_unlink(root_ext2, path); }
  return root_ramfs != NULL && ramfs_unlink(root_ramfs, path);
}

bool vfs_rename(const char *old_path, const char *new_path) {
  if (root_ext2 != NULL && !starts_with(old_path, "/dev") && !starts_with(new_path, "/dev")) {
    return ext2_rename(root_ext2, old_path, new_path);
  }
  return root_ramfs != NULL && ramfs_rename(root_ramfs, old_path, new_path);
}

uint64_t vfs_read(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len) {
  if (node->backend == VFS_RAMFS) { return ramfs_read(node->ramfs.fs, node->ramfs.index, off, dst, len); }
  if (node->backend == VFS_EXT2) {
    uint32_t got = 0;
    if (len > UINT32_MAX) { len = UINT32_MAX; }
    return ext2_read_file(root_ext2, &node->ext2, off, dst, (uint32_t)len, &got) ? got : 0;
  }
  return 0;
}

int64_t vfs_write(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len) {
  if (node->backend == VFS_EXT2) {
    struct ext2_node ext = node->ext2;
    return ext2_write_file(root_ext2, &ext, off, src, len);
  }
  if (node->backend != VFS_RAMFS) { return -28; }
  return ramfs_write(node->ramfs.fs, node->ramfs.index, off, src, len);
}

bool vfs_refresh(const struct vfs_node *node, struct vfs_node *out) {
  if (node->backend == VFS_RAMFS) {
    struct ramfs_node fresh;
    if (!ramfs_refresh_node(node->ramfs.fs, node->ramfs.index, &fresh)) { return false; }
    from_ramfs(&fresh, out);
    return true;
  }
  if (node->backend == VFS_EXT2) {
    struct ext2_node fresh;
    if (!ext2_inode(root_ext2, node->ext2.ino, &fresh)) { return false; }
    *out = *node;
    from_ext2(&fresh, out);
    out->writable = true;
    return true;
  }
  return false;
}

bool vfs_dirent(const struct vfs_node *dir, size_t index, struct vfs_dirent *out) {
  if (dir->backend == VFS_RAMFS) {
    struct ramfs_dirent ent;
    if (!ramfs_dirent(dir->ramfs.fs, dir->ramfs.index, index, &ent)) { return false; }
    *out = (struct vfs_dirent){.ino = ent.ino, .is_dir = ent.is_dir, .is_device = ent.is_device};
    kmemcpy(out->name, ent.name, kstrlen(ent.name) + 1);
    return true;
  }
  if (dir->backend == VFS_EXT2) {
    struct ext2_dirent ent;
    if (!ext2_dirent(root_ext2, &dir->ext2, index, &ent)) { return false; }
    *out = (struct vfs_dirent){.ino = ent.ino, .is_dir = ent.type == 2, .is_device = false};
    kmemcpy(out->name, ent.name, kstrlen(ent.name) + 1);
    return true;
  }
  return false;
}
