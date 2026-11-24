#pragma once

#include "fs/ext2.h"
#include "fs/ramfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  VFS_DEV_EXT2_ROOT = 0x0801,
  VFS_DEV_DEVFS = 0x0005,
  VFS_DEV_PROC = 0x0006,
  VFS_DEV_RAM0 = 0x0010,
  VFS_DEV_TMPFS = 0x0011,
  VFS_DEV_RUNFS = 0x0012,
  VFS_DEV_SYSFS = 0x0013,
  VFS_DEV_BOOT = 0x0014,
};

enum vfs_backend {
  VFS_NONE,
  VFS_RAMFS,
  VFS_EXT2,
  VFS_PROC,
};

enum {
  VFS_F_SEAL_SEAL = RAMFS_F_SEAL_SEAL,
  VFS_F_SEAL_SHRINK = RAMFS_F_SEAL_SHRINK,
  VFS_F_SEAL_GROW = RAMFS_F_SEAL_GROW,
  VFS_F_SEAL_WRITE = RAMFS_F_SEAL_WRITE,
  VFS_F_SEAL_FUTURE_WRITE = RAMFS_F_SEAL_FUTURE_WRITE,
  VFS_F_SEAL_MASK = RAMFS_F_SEAL_MASK,
};

struct vfs_node {
  enum vfs_backend backend;
  uint64_t ino;
  bool is_dir;
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
  uint64_t size;
  union {
    struct ramfs_node ramfs;
    struct ext2_node ext2;
    struct {
      int pid;
      int tid;
      int fd;
    } proc;
  };
};

struct vfs_fs_info {
  uint64_t magic;
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
  uint64_t inode_count;
  uint64_t free_inodes;
};

struct vfs_mount {
  uint64_t device;
  uint64_t parent;
  uint64_t magic;
  const char *source;
  const char *target;
  const char *fstype;
  const char *options;
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
  uint64_t page_cache_capacity;
  uint64_t page_cache_buckets;
};

void vfs_init(struct ramfs *ramfs, struct ext2_fs *ext2);
bool vfs_lookup(const char *path, struct vfs_node *out);
bool vfs_lstat(const char *path, struct vfs_node *out);
bool vfs_mkdir(const char *path, uint32_t mode);
bool vfs_create(const char *path, uint16_t mode, struct vfs_node *out);
bool vfs_mkfifo(const char *path, uint32_t mode, struct vfs_node *out);
bool vfs_mksock(const char *path, uint32_t mode, struct vfs_node *out);
bool vfs_truncate(const struct vfs_node *node, uint64_t size);
int vfs_tmpfile(const struct vfs_node *dir, uint16_t mode, uint32_t uid, uint32_t gid, bool linkable,
                struct vfs_node *out);
bool vfs_link_node(const struct vfs_node *node, const char *new_path);
bool vfs_link(const char *old_path, const char *new_path);
bool vfs_symlink(const char *target, const char *link_path);
bool vfs_readlink(const char *path, char *out, size_t cap, size_t *len_out);
bool vfs_chmod(const char *path, uint32_t mode);
bool vfs_chmod_node(const struct vfs_node *node, uint32_t mode);
bool vfs_chown(const char *path, uint32_t uid, uint32_t gid);
bool vfs_chown_node(const struct vfs_node *node, uint32_t uid, uint32_t gid);
int vfs_remove(const char *path);
bool vfs_rename(const char *old_path, const char *new_path);
bool vfs_retain_node(const struct vfs_node *node);
void vfs_release_node(const struct vfs_node *node);
bool vfs_set_sealable(const struct vfs_node *node, bool allow_sealing);
int vfs_add_seals(const struct vfs_node *node, uint32_t seals);
int vfs_get_seals(const struct vfs_node *node);
bool vfs_write_sealed(const struct vfs_node *node);
bool vfs_truncate_sealed(const struct vfs_node *node, uint64_t size);
bool vfs_shared_writable_mmap_sealed(const struct vfs_node *node);
void vfs_note_shared_writable_mapping(const struct vfs_node *node, bool add);
uint64_t vfs_read(const struct vfs_node *node, uint64_t off, void *dst, uint64_t len);
int64_t vfs_write(const struct vfs_node *node, uint64_t off, const void *src, uint64_t len);
bool vfs_retain_page_cached(const struct vfs_node *node, uint64_t off, uint64_t *pa_out);
bool vfs_refresh(const struct vfs_node *node, struct vfs_node *out);
int vfs_next_dirent(const struct vfs_node *dir, uint64_t *cursor, struct vfs_dirent *out);
bool vfs_fs_info(struct vfs_fs_info *out);
bool vfs_device_fs_info(uint64_t dev_id, struct vfs_fs_info *out);
const struct vfs_mount *vfs_mount_at(size_t index);
size_t vfs_mount_info(struct vfs_mount_info *out, size_t cap);
struct vfs_stats vfs_get_stats(void);
bool vfs_sync(void);
