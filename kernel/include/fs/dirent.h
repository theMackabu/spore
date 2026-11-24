#pragma once

#include <stdint.h>

enum vfs_dirent_type {
  VFS_DT_UNKNOWN,
  VFS_DT_FIFO,
  VFS_DT_CHR,
  VFS_DT_DIR = 4,
  VFS_DT_BLK = 6,
  VFS_DT_REG = 8,
  VFS_DT_LNK = 10,
  VFS_DT_SOCK = 12,
};

struct vfs_dirent {
  uint64_t ino;
  uint8_t type;
  uint8_t name_len;
  char name[256];
};

struct vfs_entry_name {
  uint64_t parent_dev, parent_ino;
  char name[256];
};
