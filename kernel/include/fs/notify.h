#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "proc/limits.h"

struct domain;
struct open_file;
struct trap_frame;
struct vfs_node;

enum {
  FS_ACCESS = 1, FS_MODIFY = 2, FS_ATTRIB = 4, FS_CLOSE_WRITE = 8, FS_CLOSE_NOWRITE = 16,
  FS_OPEN = 32, FS_MOVED_FROM = 64, FS_MOVED_TO = 128, FS_CREATE = 256, FS_DELETE = 512,
  FS_DELETE_SELF = 1024, FS_MOVE_SELF = 2048, FS_ONLYDIR = 1U << 24, FS_DONT_FOLLOW = 1U << 25,
};

int inotify_open(int flags);
int inotify_add(int fd, const struct vfs_node *node, uint32_t mask);
int inotify_remove(int fd, int wd);
void inotify_close(struct open_file *file);
unsigned inotify_bytes(const struct open_file *file);
int64_t inotify_read(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len, struct trap_frame *frame);

bool fsnotify_active(void);
void fsnotify_emit(uint64_t dev, uint64_t ino, uint32_t mask, uint32_t cookie, const char *name, bool unlinked);
void fsnotify_reclaim(uint64_t dev, uint64_t ino);

struct fsnotify_name {
  uint64_t parent_dev, parent_ino;
  char name[CELL_PATH_MAX];
};

struct fsnotify_entry {
  struct fsnotify_name location;
  uint64_t dev, ino;
  bool is_dir;
};

void fsnotify_opened(struct open_file *file, const char *resolved);
void fsnotify_closed(struct open_file *file);
void fsnotify_file_event(struct open_file *file, uint32_t mask);
bool fsnotify_hold_watched(const char *path, struct vfs_node *node);
void fsnotify_linked(const char *path);
void fsnotify_path_event(const char *path, uint32_t mask);
void fsnotify_capture(const char *path, struct fsnotify_entry *entry);
void fsnotify_prepare_change(const struct fsnotify_entry *entry);
void fsnotify_removed(const struct fsnotify_entry *entry);
void fsnotify_renamed(const struct fsnotify_entry *old, const struct fsnotify_entry *to);
void inotify_flush(void);
