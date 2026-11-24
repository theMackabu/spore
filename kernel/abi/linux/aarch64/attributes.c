#include "file_abi.h"
#include "abi/linux/aarch64/layout.h"
#include "abi/linux/aarch64/open.h"
#include "fs/notify.h"
#include "fs/vfs.h"
#include "proc/accounting.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/policy.h"

int64_t sys_utimensat(uint64_t dirfd, uint64_t path_addr, uint64_t times_addr, uint64_t flags) {
  (void)times_addr;
  if ((flags & ~0x100ull) != 0) { return -(int64_t)EINVAL; }
  if (path_addr == 0) {
    int fd_flags = cell_fd_get_flags((int)dirfd);
    return fd_flags < 0 ? fd_flags : 0;
  }

  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  if (!cell_fs_path_allowed(path, CELL_FS_WRITE)) { return -(int64_t)EPERM; }

  struct vfs_node node;
  if (!vfs_lookup(path, &node)) { return -(int64_t)ENOENT; }
  if (!syscall_node_access_allowed(&node, W_OK)) { return -(int64_t)EACCES; }
  return 0;
}

int64_t sys_fchmodat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t flags) {
  if ((flags & ~0x100ull) != 0) { return -(int64_t)EINVAL; }
  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  return vfs_chmod(path, (uint32_t)mode) ? 0 : -(int64_t)ENOENT;
}

int64_t sys_fchmod(uint64_t fd, uint64_t mode) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  if (!vfs_chmod_node(&node, (uint32_t)mode)) { return -EINVAL; }
  fsnotify_file_event(cell_fd_file(fd), FS_ATTRIB);
  return 0;
}

static bool chown_ids(const struct vfs_node *node, uint64_t uid_arg, uint64_t gid_arg, uint32_t *uid, uint32_t *gid) {
  *uid = uid_arg == 0xffffffffull ? node->uid : (uint32_t)uid_arg;
  *gid = gid_arg == 0xffffffffull ? node->gid : (uint32_t)gid_arg;
  return uid_arg <= 0xffffffffull && gid_arg <= 0xffffffffull;
}

int64_t sys_fchownat(uint64_t dirfd, uint64_t path_addr, uint64_t uid_arg, uint64_t gid_arg, uint64_t flags) {
  if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) { return -(int64_t)EINVAL; }
  if (cell_current_euid() != 0) { return -(int64_t)EPERM; }
  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  struct vfs_node node;
  bool nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
  if (!(nofollow ? vfs_lstat(path, &node) : vfs_lookup(path, &node))) { return -(int64_t)ENOENT; }
  uint32_t uid = 0;
  uint32_t gid = 0;
  if (!chown_ids(&node, uid_arg, gid_arg, &uid, &gid)) { return -(int64_t)EINVAL; }
  return vfs_chown(path, uid, gid) ? 0 : -(int64_t)EINVAL;
}

int64_t sys_fchown(uint64_t fd, uint64_t uid_arg, uint64_t gid_arg) {
  if (cell_current_euid() != 0) { return -(int64_t)EPERM; }
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  uint32_t uid = 0;
  uint32_t gid = 0;
  if (!chown_ids(&node, uid_arg, gid_arg, &uid, &gid)) { return -(int64_t)EINVAL; }
  if (!vfs_chown_node(&node, uid, gid)) { return -EINVAL; }
  fsnotify_file_event(cell_fd_file(fd), FS_ATTRIB);
  return 0;
}

int64_t sys_ftruncate(uint64_t fd, uint64_t size) {
  struct open_file *file = cell_fd_file(fd);
  if (!file || (file->flags & LINUX_O_PATH)) { return -EBADF; }
  if (file->type != OPEN_VFS || !linux_open_writable(file->flags) || (int64_t)size < 0 ||
      file->node.is_dir || file->node.device != RAMFS_DEV_NONE) { return -EINVAL; }
  if (vfs_truncate_sealed(&file->node, size)) { return -EPERM; }
  if (!vfs_truncate(&file->node, size)) { return -EINVAL; }
  fsnotify_file_event(file, FS_MODIFY);
  return 0;
}
