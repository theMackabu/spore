#pragma once

#include "boot_info.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  RAMFS_MAX_NODES = 96,
  RAMFS_NAME_MAX = 31,
  RAMFS_FILE_CAP = 8192,
};

enum ramfs_device {
  RAMFS_DEV_NONE,
  RAMFS_DEV_NULL,
  RAMFS_DEV_ZERO,
  RAMFS_DEV_FULL,
  RAMFS_DEV_RANDOM,
  RAMFS_DEV_URANDOM,
  RAMFS_DEV_CONSOLE,
  RAMFS_DEV_TTY,
};

struct ramfs_mem_node {
  bool used;
  bool is_dir;
  bool writable;
  enum ramfs_device device;
  int parent;
  char name[RAMFS_NAME_MAX + 1];
  const void *ro_data;
  uint8_t data[RAMFS_FILE_CAP];
  uint64_t size;
  uint64_t ino;
};

struct ramfs {
  struct ramfs_mem_node nodes[RAMFS_MAX_NODES];
  uint64_t next_ino;
};

struct ramfs_file {
  const char *path;
  const void *data;
  uint64_t size;
};

struct ramfs_node {
  struct ramfs *fs;
  int index;
  const char *name;
  const void *data;
  uint64_t size;
  uint64_t ino;
  bool is_dir;
  enum ramfs_device device;
};

struct ramfs_dirent {
  const char *name;
  uint64_t ino;
  bool is_dir;
  bool is_device;
};

void ramfs_init(struct ramfs *fs, const struct spore_boot_module *modules, uint32_t module_count, uint64_t hhdm_offset);
bool ramfs_lookup(const struct ramfs *fs, const char *path, struct ramfs_file *out);
bool ramfs_lookup_node(const struct ramfs *fs, const char *path, struct ramfs_node *out);
bool ramfs_root_dirent(size_t index, struct ramfs_dirent *out);
bool ramfs_dirent(const struct ramfs *fs, int dir_index, size_t index, struct ramfs_dirent *out);
bool ramfs_mkdir(struct ramfs *fs, const char *path);
bool ramfs_create(struct ramfs *fs, const char *path, struct ramfs_node *out);
bool ramfs_truncate(struct ramfs *fs, int index, uint64_t size);
bool ramfs_unlink(struct ramfs *fs, const char *path);
bool ramfs_rename(struct ramfs *fs, const char *old_path, const char *new_path);
uint64_t ramfs_read(struct ramfs *fs, int index, uint64_t off, void *dst, uint64_t len);
int64_t ramfs_write(struct ramfs *fs, int index, uint64_t off, const void *src, uint64_t len);
bool ramfs_refresh_node(struct ramfs *fs, int index, struct ramfs_node *out);
