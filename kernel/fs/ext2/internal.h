#pragma once

#include "fs/ext2.h"
#include "lib/mem.h"

enum {
  EXT2_SUPER_OFFSET = 1024,
  EXT2_SUPER_MAGIC = 0xef53,
  EXT2_ROOT_INO = 2,
  EXT2_S_IFDIR = 0040000,
  EXT2_S_IFLNK = 0120000,
  EXT2_S_IFREG = 0100000,
  EXT2_FT_REG_FILE = 1,
  EXT2_FT_DIR = 2,
  EXT2_FT_SYMLINK = 7,
  EXT2_INCOMPAT_FILETYPE = 0x2,
  EXT2_INCOMPAT_SPORE_EXT2PLUS = 0x80000000u,
  EXT2PLUS_MAGIC = 0x4e495053u,
  EXT2PLUS_VERSION = 1,
  EXT2PLUS_BASE_INO = 0x40000000u,
  EXT2PLUS_CHUNK_INODES = 256,
  EXT2PLUS_STATIC_INODES_BEFORE_DYNAMIC = 64,
  EXT2PLUS_FREE_NONE = UINT32_MAX,
  EXT2_DIRECT_READ_MAX = 128 * 1024,
  EXT2_DIRECT_WRITE_MAX = 128 * 1024,
  EXT2_ENOENT = 2,
  EXT2_EIO = 5,
  EXT2_ENOTEMPTY = 39,
  EXT2_EFBIG = 27,
  EXT2_ENOSPC = 28,
};

struct ext2_super {
  uint32_t inodes_count;
  uint32_t blocks_count;
  uint32_t reserved_blocks_count;
  uint32_t free_blocks_count;
  uint32_t free_inodes_count;
  uint32_t first_data_block;
  uint32_t log_block_size;
  uint32_t log_frag_size;
  uint32_t blocks_per_group;
  uint32_t frags_per_group;
  uint32_t inodes_per_group;
  uint32_t mtime;
  uint32_t wtime;
  uint16_t mnt_count;
  uint16_t max_mnt_count;
  uint16_t magic;
  uint16_t state;
  uint16_t errors;
  uint16_t minor_rev_level;
  uint32_t lastcheck;
  uint32_t checkinterval;
  uint32_t creator_os;
  uint32_t rev_level;
  uint16_t def_resuid;
  uint16_t def_resgid;
  uint32_t first_ino;
  uint16_t inode_size;
  uint16_t block_group_nr;
  uint32_t feature_compat;
  uint32_t feature_incompat;
  uint32_t feature_ro_compat;
};

struct ext2_group_desc {
  uint32_t block_bitmap;
  uint32_t inode_bitmap;
  uint32_t inode_table;
  uint16_t free_blocks_count;
  uint16_t free_inodes_count;
  uint16_t used_dirs_count;
  uint16_t pad;
  uint8_t reserved[12];
};

struct ext2_inode_disk {
  uint16_t mode;
  uint16_t uid;
  uint32_t size;
  uint32_t atime;
  uint32_t ctime;
  uint32_t mtime;
  uint32_t dtime;
  uint16_t gid;
  uint16_t links_count;
  uint32_t sectors_count;
  uint32_t flags;
  uint32_t osd1;
  uint32_t block[15];
};

struct ext2plus_header {
  uint32_t magic;
  uint32_t version;
  uint32_t header_size;
  uint32_t inode_size;
  uint32_t base_ino;
  uint32_t capacity;
  uint32_t next_index;
  uint32_t free_head;
  uint32_t free_count;
  uint32_t chunk_size;
  uint32_t static_inodes_used;
  uint32_t reserved[5];
};

extern struct ext2_stats ext2_stat_counters;

uint32_t ext2_now_sec(void);
const uint8_t *ext2_cached_block(struct ext2_fs *fs, uint32_t block);
bool ext2_read_block(struct ext2_fs *fs, uint32_t block, void *dst);
bool ext2_write_block(struct ext2_fs *fs, uint32_t block, const void *src);
bool ext2_write_blocks_direct(struct ext2_fs *fs, uint32_t first_block, const void *src, uint32_t block_count);
bool ext2_cache_range_has_dirty(struct ext2_fs *fs, uint32_t first_block, uint32_t block_count);
bool ext2_read_bytes(struct ext2_fs *fs, uint64_t offset, void *dst, uint32_t len);
bool ext2_write_bytes(struct ext2_fs *fs, uint64_t offset, const void *src, uint32_t len);
bool ext2_read_super(struct ext2_fs *fs, struct ext2_super *out);
bool ext2_write_super(struct ext2_fs *fs, const struct ext2_super *sb);
bool ext2_read_group_desc(struct ext2_fs *fs, uint32_t group, struct ext2_group_desc *out);
bool ext2_write_group_desc(struct ext2_fs *fs, uint32_t group, const struct ext2_group_desc *in);
bool ext2_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_node *out);
bool ext2_write_inode(struct ext2_fs *fs, const struct ext2_node *node);
bool ext2_file_block(struct ext2_fs *fs, const struct ext2_node *node, uint32_t file_block_index, uint32_t *block_out);
bool ext2plus_load_store(struct ext2_fs *fs, struct ext2_node *out);
bool ext2plus_read_store(struct ext2_fs *fs, uint64_t off, void *dst, uint32_t len);
bool ext2plus_write_store(struct ext2_fs *fs, uint64_t off, const void *src, uint32_t len);
uint64_t ext2plus_inode_offset(struct ext2_fs *fs, uint32_t index);
bool ext2plus_load_header(struct ext2_fs *fs, struct ext2plus_header *hdr);
bool ext2plus_write_header(struct ext2_fs *fs, const struct ext2plus_header *hdr);
int ext2_alloc_block(struct ext2_fs *fs, uint32_t *out);
bool ext2_free_block(struct ext2_fs *fs, uint32_t block);
bool ext2_alloc_inode(struct ext2_fs *fs, uint32_t *out);
bool ext2_free_inode(struct ext2_fs *fs, uint32_t ino);
bool ext2_adjust_group_used_dirs(struct ext2_fs *fs, uint32_t ino, int delta);
int ext2_file_block_for_write(struct ext2_fs *fs, struct ext2_node *node, uint32_t index, uint32_t *out,
                              bool zero_data);
void ext2_free_indirect_tree(struct ext2_fs *fs, uint32_t block, uint32_t depth);
void ext2_dir_drop_cache(struct ext2_fs *fs);
void ext2_dir_cache_invalidate_inode(struct ext2_fs *fs, uint32_t ino);
bool ext2_lookup_child(struct ext2_fs *fs, const struct ext2_node *dir, const char *name, size_t len, struct ext2_node *out);
uint32_t ext2_cache_name_block(struct ext2_fs *fs, uint32_t parent, const char *name, size_t len);
void ext2_cache_add_name(struct ext2_fs *fs, uint32_t parent_ino, const char *name, size_t len, uint32_t ino, uint32_t block_index);
void ext2_dir_cache_remove_name(struct ext2_fs *fs, uint32_t parent, const char *name);
