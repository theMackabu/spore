#pragma once

#include "vfs.h"

extern struct vfs_stats vfs_stat_counters;
extern struct ramfs *vfs_root_ramfs;
extern struct ext2_fs *vfs_root_ext2;
extern uint64_t vfs_hhdm_offset;

enum {
  VFS_DEV_EXT2_ROOT = 0x0801,
  VFS_DEV_DEVFS = 0x0005,
  VFS_DEV_PROC = 0x0006,
  VFS_DEV_RAM0 = 0x0010,
  VFS_DEV_TMPFS = 0x0011,
  VFS_DEV_RUNFS = 0x0012,
  VFS_DEV_SYSFS = 0x0013,
};

bool vfs_lookup_cache_get(const char *path, bool lstat, struct vfs_node *out);
void vfs_lookup_cache_put(const char *path, bool lstat, const struct vfs_node *node);
void vfs_lookup_cache_clear(void);

void vfs_page_cache_configure(void);
bool vfs_node_cacheable(const struct vfs_node *node);
void vfs_page_cache_invalidate_range(const struct vfs_node *node, uint64_t off, uint64_t len);
void vfs_page_cache_stats(struct vfs_stats *stats);
uint64_t vfs_read_uncached(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len);

bool vfs_path_uses_ramfs(const char *path);
bool vfs_paths_share_ramfs(const char *a, const char *b);
void vfs_update_time_sources(void);
void vfs_from_ramfs(const struct ramfs_node *node, struct vfs_node *out);
void vfs_from_ext2(const struct ext2_node *node, struct vfs_node *out);

bool vfs_proc_lookup(const char *path, struct vfs_node *out);
bool vfs_proc_readlink(const char *path, char *out, size_t cap, size_t *len_out);
bool vfs_proc_dirent(const struct vfs_node *dir, size_t index, struct vfs_dirent *out);
