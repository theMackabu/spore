#include "vfs.h"

#include "mem.h"
#include "mm/pmm.h"

#include <stddef.h>
#include <stdint.h>

static struct ramfs *root_ramfs;
static struct ext2_fs *root_ext2;
static uint64_t root_hhdm;

enum {
  EXEC_CACHE_ENTRIES = 1,
  EXEC_CACHE_MAX = 4 * 1024 * 1024,
};

struct exec_cache_entry {
  bool valid;
  char path[128];
  uint8_t *data;
  uint64_t cap;
  uint64_t size;
};

static struct exec_cache_entry exec_cache[EXEC_CACHE_ENTRIES];
static size_t exec_cache_clock;

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

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

static void copy_path(char *dst, size_t cap, const char *src) {
  size_t i = 0;
  while (i + 1 < cap && src[i] != '\0') {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

static void invalidate_exec_cache(void) {
  for (size_t i = 0; i < EXEC_CACHE_ENTRIES; ++i) {
    exec_cache[i].valid = false;
  }
}

static struct exec_cache_entry *find_exec_cache(const char *path, uint64_t size) {
  for (size_t i = 0; i < EXEC_CACHE_ENTRIES; ++i) {
    if (exec_cache[i].valid && exec_cache[i].size == size && streq(exec_cache[i].path, path)) { return &exec_cache[i]; }
  }
  return NULL;
}

static bool allocate_exec_entry(struct exec_cache_entry *entry, uint64_t need) {
  if (entry->data != NULL && need <= entry->cap) { return true; }
  if (need > EXEC_CACHE_MAX) { return false; }
  uint64_t pages = EXEC_CACHE_MAX / PAGE_SIZE;
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
  entry->data = (uint8_t *)(uintptr_t)(root_hhdm + first);
  entry->cap = pages * PAGE_SIZE;
  return true;
}

static struct exec_cache_entry *next_exec_entry(uint64_t need) {
  struct exec_cache_entry *entry = &exec_cache[exec_cache_clock++ % EXEC_CACHE_ENTRIES];
  entry->valid = false;
  return allocate_exec_entry(entry, need) ? entry : NULL;
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
      struct exec_cache_entry *entry = find_exec_cache(path, node.size);
      if (entry != NULL) {
        *data = entry->data;
        *size = entry->size;
        return true;
      }
      entry = next_exec_entry(node.size);
      if (entry == NULL) { return false; }
      uint32_t got = 0;
      if (!ext2_read_file(root_ext2, &node, 0, entry->data, node.size, &got) || got != node.size) { return false; }
      copy_path(entry->path, sizeof(entry->path), path);
      entry->size = node.size;
      entry->valid = true;
      *data = entry->data;
      *size = entry->size;
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
    bool ok = ext2_create(root_ext2, path, true, &node);
    if (ok) { invalidate_exec_cache(); }
    return ok;
  }
  return root_ramfs != NULL && ramfs_mkdir(root_ramfs, path);
}

bool vfs_create(const char *path, struct vfs_node *out) {
  if (root_ext2 != NULL && !starts_with(path, "/dev")) {
    struct ext2_node node;
    if (!ext2_create(root_ext2, path, false, &node)) { return false; }
    from_ext2(&node, out);
    out->writable = true;
    invalidate_exec_cache();
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
    bool ok = ext2_truncate(root_ext2, &ext, size);
    if (ok) { invalidate_exec_cache(); }
    return ok;
  }
  if (node->backend != VFS_RAMFS) { return false; }
  return ramfs_truncate(node->ramfs.fs, node->ramfs.index, size);
}

bool vfs_unlink(const char *path) {
  if (root_ext2 != NULL && !starts_with(path, "/dev")) {
    bool ok = ext2_unlink(root_ext2, path);
    if (ok) { invalidate_exec_cache(); }
    return ok;
  }
  return root_ramfs != NULL && ramfs_unlink(root_ramfs, path);
}

bool vfs_rename(const char *old_path, const char *new_path) {
  if (root_ext2 != NULL && !starts_with(old_path, "/dev") && !starts_with(new_path, "/dev")) {
    bool ok = ext2_rename(root_ext2, old_path, new_path);
    if (ok) { invalidate_exec_cache(); }
    return ok;
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
    int64_t wrote = ext2_write_file(root_ext2, &ext, off, src, len);
    if (wrote >= 0) { invalidate_exec_cache(); }
    return wrote;
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
