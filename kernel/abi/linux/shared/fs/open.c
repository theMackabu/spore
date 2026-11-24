#include "handlers.h"
#include "abi/linux/shared/usercopy.h"
#include "abi/linux/shared/fs/file_abi.h"
#include "abi/linux/shared/open.h"
#include "fs/notify.h"
#include "fs/vfs.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/policy.h"

enum {
  O_CREAT = 0100,
  O_EXCL = 0200,
  O_TRUNC = 01000,
#ifdef SPORE_NATIVE_PORT
  O_DIRECTORY = 00200000,
  O_NOFOLLOW = 00400000,
#else
  O_DIRECTORY = 040000,
  O_NOFOLLOW = 0100000,
#endif
};

static size_t append_literal(char *dst, size_t cap, size_t len, const char *text) {
  while (*text != '\0' && len + 1 < cap) {
    dst[len++] = *text++;
  }
  if (cap != 0) { dst[len] = '\0'; }
  return len;
}

static size_t append_u64(char *dst, size_t cap, size_t len, uint64_t value) {
  char tmp[20];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  } while (value != 0 && n < sizeof(tmp));
  while (n > 0 && len + 1 < cap) {
    dst[len++] = tmp[--n];
  }
  if (cap != 0) { dst[len] = '\0'; }
  return len;
}

int64_t sys_openat(struct trap_frame *frame, uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mode) {
  struct syscall_path name;
  int64_t rc = syscall_read_path_at(dirfd, path_addr, &name);
  if (rc) { return rc; }
  char *path = name.resolved;
  const char *display = name.visible;
  bool path_only = flags & LINUX_O_PATH;
  if (path_only) { flags &= LINUX_O_PATH | O_DIRECTORY | O_NOFOLLOW | LINUX_O_CLOEXEC; }
  uint64_t access = linux_open_readable(flags) ? R_OK : 0;
  if (linux_open_writable(flags) || (flags & O_TRUNC)) { access |= W_OK; }
  uint8_t rights = syscall_fs_rights_from_access(access);
  if (flags & (O_CREAT | O_TMPFILE_BIT)) { rights |= CELL_FS_WRITE; }
  if (!cell_fs_path_allowed(path, rights)) { return -EPERM; }
  if ((flags & O_TMPFILE_BIT) != 0) {
    if ((flags & (O_CREAT | O_DIRECTORY)) != O_DIRECTORY ||
        ((flags & LINUX_O_ACCMODE) != LINUX_O_RDWR && (flags & LINUX_O_ACCMODE) != LINUX_O_WRONLY)) {
      return -EINVAL;
    }
    struct vfs_node dir, tmp;
    if (!vfs_lookup(path, &dir)) { return -ENOENT; }
    if (!dir.is_dir) { return -ENOTDIR; }
    if (!syscall_node_access_allowed(&dir, W_OK | X_OK)) { return -EACCES; }
    int rc = vfs_tmpfile(&dir, cell_creation_mode(mode), cell_current_euid(), cell_current_egid(),
                         (flags & O_EXCL) == 0, &tmp);
    if (rc < 0) { return rc; }
    size_t len = kstrlen(name.visible);
    len = append_literal(name.visible, sizeof(name.visible), len, len == 1 ? "#" : "/#");
    len = append_u64(name.visible, sizeof(name.visible), len, tmp.ino);
    append_literal(name.visible, sizeof(name.visible), len, " (deleted)");
    int fd = cell_fd_open_node(&tmp, flags, name.visible, NULL, NULL);
    vfs_release_node(&tmp);
    return fd;
  }
  bool exclusive = (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
  bool nofollow = (flags & O_NOFOLLOW) && !name.trailing_slash;
  bool directory = (flags & O_DIRECTORY) || name.trailing_slash;
  if ((flags & (O_DIRECTORY | O_CREAT)) == (O_DIRECTORY | O_CREAT)) { return -EINVAL; }
  int proc_fd;
  struct vfs_node node;
  bool exists;
  if (!nofollow && syscall_proc_fd_number(path, &proc_fd)) {
    struct open_file *target = cell_file_at(cell_current_domain_internal(), proc_fd);
    if (!target) { return -ENOENT; }
    if (exclusive) { return -EEXIST; }
    if (directory && !cell_fd_is_dir(proc_fd)) { return -ENOTDIR; }
    if (target->type != OPEN_VFS) {
      if (path_only) { return -6; }
      int fd = cell_fd_dup(proc_fd, 0);
      if (fd >= 0 && (flags & LINUX_O_CLOEXEC)) { cell_fd_set_fd_flags(fd, 1); }
      return fd;
    }
    if (!cell_fd_stat(proc_fd, &node)) { return -ENOENT; }
    exists = true;
    display = target->path;
  } else {
    exists = (nofollow || exclusive) ? vfs_lstat(path, &node) : vfs_lookup(path, &node);
  }
  if (!exists) {
    if (!(flags & O_CREAT)) { return -ENOENT; }
    if (directory) { return -EISDIR; }
    rc = syscall_may_change_entry(path, false);
    if (rc) { return rc; }
    if (!vfs_create(path, cell_creation_mode(mode), &node)) { return -ENOENT; }
    (void)vfs_chown_node(&node, cell_current_euid(), cell_current_egid());
    if (!vfs_refresh(&node, &node)) { return -EIO; }
  } else {
    if ((access & W_OK) && vfs_readonly_node(&node)) { return -30; }
    if (exclusive) { return -EEXIST; }
    if (nofollow && !path_only && (node.mode & S_IFMT) == S_IFLNK) { return -ELOOP; }
    if (directory && !node.is_dir) { return -ENOTDIR; }
    if (node.is_dir && ((flags & (O_CREAT | O_TRUNC)) || linux_open_writable(flags))) { return -EISDIR; }
    if (!syscall_node_access_allowed(&node, access)) { return -EACCES; }
    if ((flags & O_TRUNC) && (node.mode & S_IFMT) == S_IFREG && node.device == RAMFS_DEV_NONE) {
      if (vfs_truncate_sealed(&node, 0)) { return -EPERM; }
      if (!vfs_truncate(&node, 0) || !vfs_refresh(&node, &node)) { return -EIO; }
      fsnotify_node_event(&node, path, FS_MODIFY);
    }
  }
  return cell_fd_open_node(&node, (uint32_t)flags, display, path, frame);
}
