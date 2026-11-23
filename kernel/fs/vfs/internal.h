#pragma once

#include "fs/vfs.h"

extern struct vfs_stats vfs_stat_counters;
extern struct ramfs *vfs_root_ramfs;
extern struct ext2_fs *vfs_root_ext2;
extern uint64_t vfs_hhdm_offset;

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
