#pragma once

#include "ext2.h"
#include "ramfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum vfs_backend {
  VFS_NONE,
  VFS_RAMFS,
  VFS_EXT2,
};

struct vfs_node {
  enum vfs_backend backend;
  uint64_t ino;
  bool is_dir;
  bool writable;
  enum ramfs_device device;
  uint64_t size;
  struct ramfs_node ramfs;
  struct ext2_node ext2;
};

struct vfs_dirent {
  uint64_t ino;
  bool is_dir;
  bool is_device;
  char name[256];
};

void vfs_init(struct ramfs *ramfs, struct ext2_fs *ext2, uint64_t hhdm_offset);
bool vfs_lookup(const char *path, struct vfs_node *out);
bool vfs_lookup_exec(const char *path, const void **data, uint64_t *size);
bool vfs_mkdir(const char *path);
bool vfs_create(const char *path, struct vfs_node *out);
bool vfs_truncate(const struct vfs_node *node, uint64_t size);
bool vfs_unlink(const char *path);
bool vfs_rename(const char *old_path, const char *new_path);
uint64_t vfs_read(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len);
int64_t vfs_write(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len);
bool vfs_refresh(const struct vfs_node *node, struct vfs_node *out);
bool vfs_dirent(const struct vfs_node *dir, size_t index, struct vfs_dirent *out);
