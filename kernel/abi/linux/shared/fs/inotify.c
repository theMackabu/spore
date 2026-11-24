#include "file_abi.h"
#include "fs/notify.h"
#include "fs/vfs.h"

int64_t sys_inotify_init1(uint64_t flags) {
  return inotify_open((int)flags);
}

int64_t sys_inotify_add_watch(uint64_t fd, uint64_t path_addr, uint64_t mask) {
  char path[CELL_PATH_MAX];
  int64_t rc = syscall_copy_resolved_path_at(AT_FDCWD, path_addr, path, sizeof(path));
  if (rc) { return rc; }
  struct vfs_node node;
  bool exists = mask & FS_DONT_FOLLOW ? vfs_lstat(path, &node) : vfs_lookup(path, &node);
  if (!exists) { return -ENOENT; }
  if (!syscall_node_access_allowed(&node, 4)) { return -13; }
  return inotify_add((int)fd, &node, (uint32_t)mask);
}

int64_t sys_inotify_rm_watch(uint64_t fd, uint64_t wd) {
  return inotify_remove((int)fd, (int)wd);
}
