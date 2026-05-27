#pragma once

#include "boot_info.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  RAMFS_MAX_NODES = 128,
  RAMFS_NAME_MAX = 31,
  RAMFS_FILE_CAP = 128 * 1024 * 1024,
  RAMFS_PAGE_SIZE = 4096,
  RAMFS_MAX_BACKING_PAGES = RAMFS_FILE_CAP / RAMFS_PAGE_SIZE,
};

enum ramfs_mount {
  RAMFS_MOUNT_RAM0,
  RAMFS_MOUNT_DEV,
  RAMFS_MOUNT_PROC,
  RAMFS_MOUNT_TMP,
  RAMFS_MOUNT_RUN,
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
  RAMFS_DEV_PROCINFO,
  RAMFS_DEV_MEMINFO,
  RAMFS_DEV_CPUINFO,
  RAMFS_DEV_UPTIME,
  RAMFS_DEV_LOADAVG,
  RAMFS_DEV_MOUNTS,
  RAMFS_DEV_STAT,
  RAMFS_DEV_NET_DEV,
  RAMFS_DEV_KMSG,
  RAMFS_DEV_FILESYSTEMS,
  RAMFS_DEV_PARTITIONS,
  RAMFS_DEV_DEVICES,
  RAMFS_DEV_FSSTATS,
  RAMFS_DEV_PROC_PID_STAT,
  RAMFS_DEV_PROC_PID_STATUS,
  RAMFS_DEV_PROC_PID_CMDLINE,
  RAMFS_DEV_PROC_PID_STATM,
  RAMFS_DEV_PROC_PID_COMM,
  RAMFS_DEV_PROC_PID_MOUNTS,
  RAMFS_DEV_PROC_PID_CWD,
  RAMFS_DEV_PROC_PID_EXE,
  RAMFS_DEV_FS_ROOT,
  RAMFS_DEV_FS_BOOT,
  RAMFS_DEV_FS_RAM0,
  RAMFS_DEV_FS_TMP,
  RAMFS_DEV_BLK_ROOT,
  RAMFS_DEV_BLK_BOOT,
};

struct ramfs_mem_node {
  bool used;
  bool is_dir;
  bool writable;
  enum ramfs_mount mount;
  enum ramfs_device device;
  uint16_t mode;
  uint32_t uid;
  uint32_t gid;
  int first_page;
  int parent;
  char name[RAMFS_NAME_MAX + 1];
  const void *ro_data;
  uint64_t size;
  uint64_t ino;
  uint64_t atime;
  uint64_t ctime;
  uint64_t mtime;
};

struct ramfs {
  struct ramfs_mem_node nodes[RAMFS_MAX_NODES];
  uint64_t hhdm_offset;
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
  enum ramfs_mount mount;
  enum ramfs_device device;
  uint16_t mode;
  uint32_t uid;
  uint32_t gid;
  uint64_t atime;
  uint64_t ctime;
  uint64_t mtime;
};

struct ramfs_dirent {
  const char *name;
  uint64_t ino;
  bool is_dir;
  bool is_device;
  uint8_t type;
};

void ramfs_init(struct ramfs *fs, const struct spore_boot_module *modules, uint32_t module_count, uint64_t hhdm_offset);
void ramfs_set_now(uint64_t epoch_sec);
bool ramfs_lookup(const struct ramfs *fs, const char *path, struct ramfs_file *out);
bool ramfs_lookup_node(const struct ramfs *fs, const char *path, struct ramfs_node *out);
bool ramfs_root_dirent(size_t index, struct ramfs_dirent *out);
bool ramfs_dirent(const struct ramfs *fs, int dir_index, size_t index, struct ramfs_dirent *out);
bool ramfs_mkdir(struct ramfs *fs, const char *path);
bool ramfs_create(struct ramfs *fs, const char *path, struct ramfs_node *out);
bool ramfs_mkfifo(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out);
bool ramfs_mksock(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out);
bool ramfs_truncate(struct ramfs *fs, int index, uint64_t size);
bool ramfs_chmod_node(struct ramfs *fs, int index, uint16_t mode);
bool ramfs_chown_node(struct ramfs *fs, int index, uint32_t uid, uint32_t gid);
bool ramfs_unlink(struct ramfs *fs, const char *path);
bool ramfs_link(struct ramfs *fs, const char *old_path, const char *new_path);
bool ramfs_rename(struct ramfs *fs, const char *old_path, const char *new_path);
uint64_t ramfs_read(struct ramfs *fs, int index, uint64_t off, void *dst, uint64_t len);
int64_t ramfs_write(struct ramfs *fs, int index, uint64_t off, const void *src, uint64_t len);
bool ramfs_refresh_node(struct ramfs *fs, int index, struct ramfs_node *out);
uint64_t ramfs_backing_used_pages(void);
