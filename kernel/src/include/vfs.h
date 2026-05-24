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
  VFS_PROC,
};

struct vfs_node {
  enum vfs_backend backend;
  uint64_t ino;
  bool is_dir;
  bool writable;
  enum ramfs_device device;
  uint16_t mode;
  uint16_t links_count;
  uint32_t uid;
  uint32_t gid;
  uint64_t dev_id;
  uint64_t rdev;
  int proc_pid;
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

struct vfs_fs_info {
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
  uint64_t inode_count;
  uint64_t free_inodes;
};

struct vfs_mount_info {
  char source[32];
  char target[32];
  char fstype[16];
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
};

void vfs_init(struct ramfs *ramfs, struct ext2_fs *ext2, uint64_t hhdm_offset);
bool vfs_lookup(const char *path, struct vfs_node *out);
bool vfs_lstat(const char *path, struct vfs_node *out);
bool vfs_lookup_exec(const char *path, const void **data, uint64_t *size);
bool vfs_mkdir(const char *path);
bool vfs_create(const char *path, struct vfs_node *out);
bool vfs_truncate(const struct vfs_node *node, uint64_t size);
bool vfs_link(const char *old_path, const char *new_path);
bool vfs_symlink(const char *target, const char *link_path);
bool vfs_readlink(const char *path, char *out, size_t cap, size_t *len_out);
bool vfs_chmod(const char *path, uint32_t mode);
bool vfs_chmod_node(const struct vfs_node *node, uint32_t mode);
bool vfs_chown(const char *path, uint32_t uid, uint32_t gid);
bool vfs_chown_node(const struct vfs_node *node, uint32_t uid, uint32_t gid);
bool vfs_unlink(const char *path);
bool vfs_rename(const char *old_path, const char *new_path);
uint64_t vfs_read(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len);
int64_t vfs_write(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len);
bool vfs_refresh(const struct vfs_node *node, struct vfs_node *out);
bool vfs_dirent(const struct vfs_node *dir, size_t index, struct vfs_dirent *out);
bool vfs_next_dirent(const struct vfs_node *dir, uint64_t *cursor, struct vfs_dirent *out);
bool vfs_fs_info(struct vfs_fs_info *out);
size_t vfs_mount_info(struct vfs_mount_info *out, size_t cap);
uint64_t vfs_exec_cache_pages(void);
