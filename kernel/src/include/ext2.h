#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  EXT2_NAME_MAX = 255,
};

typedef bool (*ext2_read_fn)(void *ctx, uint64_t offset, void *dst, uint32_t len);
typedef bool (*ext2_write_fn)(void *ctx, uint64_t offset, const void *src, uint32_t len);

struct ext2_fs {
  ext2_read_fn read;
  ext2_write_fn write;
  void *ctx;
  uint32_t block_size;
  uint32_t inodes_per_group;
  uint32_t blocks_per_group;
  uint32_t inode_size;
  uint32_t first_data_block;
  uint32_t group_count;
  uint32_t inode_count;
  uint32_t block_count;
};

struct __attribute__((aligned(8))) ext2_node {
  uint32_t ino;
  uint16_t mode;
  uint16_t links_count;
  uint32_t size;
  uint32_t sectors_count;
  uint32_t blocks[15];
};

struct ext2_dirent {
  uint32_t ino;
  uint8_t type;
  char name[EXT2_NAME_MAX + 1];
};

struct ext2_info {
  uint32_t block_size;
  uint32_t block_count;
  uint32_t free_blocks;
  uint32_t inode_count;
  uint32_t free_inodes;
};

bool ext2_mount(struct ext2_fs *fs, ext2_read_fn read, void *ctx);
bool ext2_mount_rw(struct ext2_fs *fs, ext2_read_fn read, ext2_write_fn write, void *ctx);
bool ext2_lookup(struct ext2_fs *fs, const char *path, struct ext2_node *out);
bool ext2_lstat(struct ext2_fs *fs, const char *path, struct ext2_node *out);
bool ext2_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out);
bool ext2_read_file(struct ext2_fs *fs, const struct ext2_node *node, uint64_t off, void *dst, uint32_t len,
                    uint32_t *read_out);
int64_t ext2_write_file(struct ext2_fs *fs, struct ext2_node *node, uint64_t off, const void *src, uint64_t len);
bool ext2_truncate(struct ext2_fs *fs, struct ext2_node *node, uint64_t size);
bool ext2_create(struct ext2_fs *fs, const char *path, bool dir, struct ext2_node *out);
bool ext2_link(struct ext2_fs *fs, const char *old_path, const char *new_path);
bool ext2_symlink(struct ext2_fs *fs, const char *target, const char *link_path);
bool ext2_readlink(struct ext2_fs *fs, const char *path, char *out, size_t cap, size_t *len_out);
bool ext2_chmod(struct ext2_fs *fs, const char *path, uint32_t mode);
bool ext2_chmod_node(struct ext2_fs *fs, const struct ext2_node *node, uint32_t mode);
bool ext2_unlink(struct ext2_fs *fs, const char *path);
bool ext2_rename(struct ext2_fs *fs, const char *old_path, const char *new_path);
bool ext2_dirent(struct ext2_fs *fs, const struct ext2_node *dir, size_t index, struct ext2_dirent *out);
bool ext2_next_dirent(struct ext2_fs *fs, const struct ext2_node *dir, uint64_t *cursor, struct ext2_dirent *out);
bool ext2_is_dir(const struct ext2_node *node);
bool ext2_is_regular(const struct ext2_node *node);
bool ext2_is_symlink(const struct ext2_node *node);
bool ext2_info(struct ext2_fs *fs, struct ext2_info *out);
uint64_t ext2_cache_used_pages(void);
