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
  uint64_t atime;
  uint64_t ctime;
  uint64_t mtime;
  int proc_pid;
  uint64_t size;
  struct ramfs_node ramfs;
  struct ext2_node ext2;
};

struct vfs_dirent {
  uint64_t ino;
  bool is_dir;
  bool is_device;
  uint8_t type;
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

struct vfs_stats {
  uint64_t lookup_count;
  uint64_t lstat_count;
  uint64_t lookup_cache_hits;
  uint64_t lookup_cache_misses;
  uint64_t lookup_cache_invalidations;
  uint64_t dirent_count;
  uint64_t next_dirent_count;
  uint64_t page_cache_hits;
  uint64_t page_cache_misses;
  uint64_t page_cache_loads;
  uint64_t page_cache_invalidations;
  uint64_t page_cache_pages;
};

void vfs_init(struct ramfs *ramfs, struct ext2_fs *ext2, uint64_t hhdm_offset);
bool vfs_lookup(const char *path, struct vfs_node *out);
bool vfs_lstat(const char *path, struct vfs_node *out);
bool vfs_mkdir(const char *path, uint32_t mode);
bool vfs_create(const char *path, struct vfs_node *out);
bool vfs_mkfifo(const char *path, uint32_t mode, struct vfs_node *out);
bool vfs_mksock(const char *path, uint32_t mode, struct vfs_node *out);
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
bool vfs_read_page_cached(const struct vfs_node *node, uint64_t off, void *dst);
bool vfs_retain_page_cached(const struct vfs_node *node, uint64_t off, uint64_t *pa_out);
void vfs_page_cache_invalidate(const struct vfs_node *node);
bool vfs_refresh(const struct vfs_node *node, struct vfs_node *out);
bool vfs_dirent(const struct vfs_node *dir, size_t index, struct vfs_dirent *out);
bool vfs_next_dirent(const struct vfs_node *dir, uint64_t *cursor, struct vfs_dirent *out);
bool vfs_fs_info(struct vfs_fs_info *out);
size_t vfs_mount_info(struct vfs_mount_info *out, size_t cap);
struct vfs_stats vfs_get_stats(void);
