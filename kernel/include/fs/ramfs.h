#pragma once

#include "fs/dirent.h"

#include "init/boot_info.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  RAMFS_NAME_MAX = 255,
  RAMFS_PAGE_SIZE = 4096,
  RAMFS_INITIAL_BACKING_PAGES = 256,
  RAMFS_MAX_BACKING_PAGES = (4ull * 1024 * 1024 * 1024) / RAMFS_PAGE_SIZE,
  RAMFS_F_SEAL_SEAL = 0x0001,
  RAMFS_F_SEAL_SHRINK = 0x0002,
  RAMFS_F_SEAL_GROW = 0x0004,
  RAMFS_F_SEAL_WRITE = 0x0008,
  RAMFS_F_SEAL_FUTURE_WRITE = 0x0010,
  RAMFS_F_SEAL_MASK = 0x001f,
};

constexpr uint64_t RAMFS_MAX_FILE_SIZE = (uint64_t)UINT32_MAX * RAMFS_PAGE_SIZE;

enum ramfs_mount {
  RAMFS_MOUNT_RAM0,
  RAMFS_MOUNT_DEV,
  RAMFS_MOUNT_PROC,
  RAMFS_MOUNT_SYS,
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
  RAMFS_DEV_PTMX,
  RAMFS_DEV_PTS,
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
  RAMFS_DEV_PROC_PID_MOUNTINFO,
  RAMFS_DEV_PROC_PID_CWD,
  RAMFS_DEV_PROC_PID_EXE,
  RAMFS_DEV_PROC_PID_FD,
  RAMFS_DEV_FS_ROOT,
  RAMFS_DEV_FS_BOOT,
  RAMFS_DEV_FS_RAM0,
  RAMFS_DEV_FS_TMP,
  RAMFS_DEV_SYS_CPU_POSSIBLE,
  RAMFS_DEV_SYS_CPU_PRESENT,
  RAMFS_DEV_SYS_CPU_ONLINE,
  RAMFS_DEV_SYS_CPU_CORE_ONLINE,
  RAMFS_DEV_BLK_ROOT,
  RAMFS_DEV_BLK_BOOT,
  RAMFS_DEV_PROC_PID_TASK,
  RAMFS_DEV_PROC_SELF,
};

constexpr uint64_t RAMFS_DIR_END = UINT64_C(1) << 32;

struct ramfs_table;
struct ramfs {
  struct ramfs_table *table;
  uint64_t hhdm_offset;
  uint64_t next_ino;
};

struct ramfs_node {
  struct ramfs *fs;
  int index;
  uint32_t allocated_pages;
  const char *name;
  uint64_t size;
  uint64_t ino;
  bool is_dir;
  enum ramfs_mount mount;
  enum ramfs_device device;
  uint16_t mode;
  uint32_t uid;
  uint32_t gid;
  uint32_t links_count;
  uint64_t atime;
  uint64_t ctime;
  uint64_t mtime;
};

typedef void (*ramfs_reclaim_fn)(enum ramfs_mount mount, uint64_t ino);
void ramfs_set_reclaim_callback(struct ramfs *fs, ramfs_reclaim_fn callback);
void ramfs_destroy(struct ramfs *fs);
bool ramfs_init(struct ramfs *fs, const struct spore_boot_module *modules, uint32_t module_count, uint64_t hhdm_offset);
void ramfs_set_now(uint64_t epoch_sec);
bool ramfs_lookup_node(const struct ramfs *fs, const char *path, struct ramfs_node *out);
bool ramfs_next_dirent(const struct ramfs *fs, int dir_index, uint64_t *cursor, struct vfs_dirent *out);
bool ramfs_mkdir(struct ramfs *fs, const char *path, uint16_t mode);
bool ramfs_create(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out);
bool ramfs_mkfifo(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out);
bool ramfs_mksock(struct ramfs *fs, const char *path, uint16_t mode, struct ramfs_node *out);
bool ramfs_truncate(struct ramfs *fs, int index, uint64_t size);
bool ramfs_set_times(struct ramfs *fs, int index, uint64_t atime, uint64_t mtime);
bool ramfs_chmod_node(struct ramfs *fs, int index, uint16_t mode);
bool ramfs_chown_node(struct ramfs *fs, int index, uint32_t uid, uint32_t gid);
bool ramfs_retain_node(struct ramfs *fs, int index);
void ramfs_release_node(struct ramfs *fs, int index);
bool ramfs_set_sealable(struct ramfs *fs, int index, bool allow_sealing);
int ramfs_add_seals(struct ramfs *fs, int index, uint32_t seals);
int ramfs_get_seals(struct ramfs *fs, int index);
bool ramfs_write_sealed(struct ramfs *fs, int index);
bool ramfs_truncate_sealed(struct ramfs *fs, int index, uint64_t size);
void ramfs_note_shared_writable_mapping(struct ramfs *fs, int index, bool add);
int ramfs_remove(struct ramfs *fs, const char *path);
static inline bool ramfs_unlink(struct ramfs *fs, const char *path) {
  return ramfs_remove(fs, path) == 0;
}
bool ramfs_tmpfile(const struct ramfs_node *dir, uint16_t mode, uint32_t uid, uint32_t gid, bool linkable,
                   struct ramfs_node *out);
bool ramfs_link_node(struct ramfs *fs, int index, const char *new_path);
bool ramfs_link(struct ramfs *fs, const char *old_path, const char *new_path);
int ramfs_renameat(struct ramfs *fs, const char *old_path, const char *new_path, bool noreplace);
static inline bool ramfs_rename(struct ramfs *fs, const char *old_path, const char *new_path) {
  return ramfs_renameat(fs, old_path, new_path, false) == 0;
}
uint64_t ramfs_read(struct ramfs *fs, int index, uint64_t off, void *dst, uint64_t len);
int64_t ramfs_write(struct ramfs *fs, int index, uint64_t off, const void *src, uint64_t len);
int64_t ramfs_writeback(struct ramfs *fs, int index, uint64_t off, const void *src, uint64_t len);
bool ramfs_refresh_node(struct ramfs *fs, int index, struct ramfs_node *out);
uint64_t ramfs_backing_used_pages(void);
