#pragma once

#include "fs/vfs.h"
#include "fs/mount_namespace.h"

extern struct vfs_stats vfs_stat_counters;
extern struct ramfs *vfs_root_ramfs;
extern struct ext2_fs *vfs_root_ext2;

bool vfs_lookup_cache_get(const char *path, bool lstat, struct vfs_node *out);
void vfs_lookup_cache_put(const char *path, bool lstat, const struct vfs_node *node);
void vfs_lookup_cache_clear(void);
void vfs_lookup_cache_changed(const struct vfs_node *node);

void vfs_ramfs_inode_freed(struct ramfs *fs, enum ramfs_mount mount, uint64_t ino);
void vfs_page_cache_configure(void);
bool vfs_sync_mapped_pages(void);
bool vfs_node_cacheable(const struct vfs_node *node);
uint64_t vfs_read_cached(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len);
void vfs_page_cache_forget(const struct vfs_node *node);
void vfs_page_cache_truncate(const struct vfs_node *node, uint64_t size);
void vfs_page_cache_write(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len);
void vfs_page_cache_stats(struct vfs_stats *stats);
uint64_t vfs_read_uncached(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len);

const char *vfs_disk_path(const char *path);
bool vfs_path_uses_ramfs(const char *path);
bool vfs_paths_share_ramfs(const char *a, const char *b);
void vfs_update_time_sources(void);
void vfs_from_ramfs(const struct ramfs_node *node, struct vfs_node *out);
void vfs_from_ext2(const struct ext2_node *node, struct vfs_node *out);
