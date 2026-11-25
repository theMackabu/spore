#include "../usercopy.h"
#include "file_abi.h"
#include "abi/linux/shared/open.h"
#include "fs/vfs.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/capabilities.h"
#include "proc/fd.h"
#include "proc/policy.h"

int64_t sys_getcwd(uint64_t buf, uint64_t len) {
  const char *cwd = cell_current_cwd();
  size_t need = kstrlen(cwd) + 1;
  if (len < need) { return -34; }
  if (!syscall_user_writable(buf, need)) { return -EFAULT; }
  return vmm_copy_to_user(syscall_active_as(), buf, cwd, need) ? (int64_t)need : -EFAULT;
}

int64_t sys_chdir(uint64_t path_addr) {
  struct syscall_path name;
  int64_t rc = syscall_read_path_at(AT_FDCWD, path_addr, &name);
  if (rc) { return rc; }
  struct vfs_node node;
  if (!vfs_lookup(name.resolved, &node)) { return -(int64_t)ENOENT; }
  if (!node.is_dir) { return -(int64_t)ENOTDIR; }
  if (!syscall_node_access_allowed(&node, X_OK)) { return -EACCES; }
  return cell_set_cwd(name.visible) ? 0 : -(int64_t)ENAMETOOLONG;
}

int64_t sys_chroot(uint64_t path_addr) {
  if (!cell_capable(CAP_SYS_CHROOT)) { return -EPERM; }
  char path[CELL_PATH_MAX];
  int64_t rc = syscall_copy_resolved_path_at(AT_FDCWD, path_addr, path, sizeof(path));
  if (rc) { return rc; }
  struct vfs_node node;
  if (!vfs_lookup(path, &node)) { return -(int64_t)ENOENT; }
  if (!node.is_dir) { return -(int64_t)ENOTDIR; }
  if (!cell_set_chroot(path) || !cell_set_cwd("/")) { return -(int64_t)ENAMETOOLONG; }
  return 0;
}

int64_t sys_mkdirat(uint64_t dirfd, uint64_t path_addr, uint64_t mode) {
  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  path_rc = syscall_may_change_entry(path, false);
  if (path_rc) { return path_rc; }
  if (!vfs_mkdir(path, cell_creation_mode(mode))) {
    struct vfs_node existing;
    if (vfs_lookup(path, &existing)) { return -(int64_t)EEXIST; }
    return -(int64_t)EINVAL;
  }
  (void)vfs_chown(path, cell_current_euid(), cell_current_egid());
  return 0;
}

int64_t sys_mknodat(uint64_t dirfd, uint64_t path_addr, uint64_t mode) {
  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  if ((mode & S_IFMT) != S_IFIFO) { return -(int64_t)EINVAL; }
  struct vfs_node node;
  path_rc = syscall_may_change_entry(path, false);
  if (path_rc) { return path_rc; }
  int rc = vfs_mkspecial(path, S_IFIFO | cell_creation_mode(mode), &node);
  if (rc) { return rc; }
  (void)vfs_chown_node(&node, cell_current_euid(), cell_current_egid());
  return 0;
}

int64_t sys_unlinkat(uint64_t dirfd, uint64_t path_addr, uint64_t flags) {
  constexpr uint64_t AT_REMOVEDIR = 0x200;
  if ((flags & ~AT_REMOVEDIR) != 0) { return -(int64_t)EINVAL; }
  bool directory = (flags & AT_REMOVEDIR) != 0;
  char raw[CELL_PATH_MAX];
  int64_t size = syscall_read_user_string(path_addr, raw, sizeof(raw));
  if (size < 0) { return size == -E2BIG ? -ENAMETOOLONG : size; }
  if (size == 1) { return -ENOENT; }
  size_t end = (size_t)size - 1;
  bool trailing_slash = raw[end - 1] == '/';
  while (end && raw[end - 1] == '/') { --end; }
  if (!end) { return directory ? -EBUSY : -EISDIR; }
  raw[end] = '\0';
  size_t start = end;
  while (start && raw[start - 1] != '/') { --start; }
  bool dot = str_eq(raw + start, ".");
  bool dotdot = str_eq(raw + start, "..");
  if (dot || dotdot) { raw[start] = '\0'; }

  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_resolve_path_at(dirfd, raw[0] ? raw : ".", path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  struct vfs_node node;
  if (dot || dotdot) {
    if (!vfs_lookup(path, &node)) { return -ENOENT; }
    if (!node.is_dir) { return -ENOTDIR; }
    if (!directory) { return -EISDIR; }
    return dot ? -EINVAL : -ENOTEMPTY;
  }
  if (!vfs_lstat(path, &node)) { return -ENOENT; }
  if (!node.is_dir && (directory || trailing_slash)) { return -ENOTDIR; }
  if (node.is_dir && !directory) { return -EISDIR; }
  const struct vfs_mount *mount;
  for (size_t i = 0; (mount = vfs_mount_at(i)) != NULL; ++i) {
    if (str_eq(path, mount->target)) { return -EBUSY; }
  }
  path_rc = syscall_may_change_entry(path, true);
  return path_rc ? path_rc : vfs_remove(path);
}

static int64_t rename_path(uint64_t dirfd, uint64_t addr, char path[CELL_PATH_MAX], bool *trailing_slash) {
  char raw[CELL_PATH_MAX];
  int64_t len = syscall_read_user_string(addr, raw, sizeof(raw));
  if (len < 0) { return len == -E2BIG ? -ENAMETOOLONG : len; }
  if (len == 1) { return -ENOENT; }
  size_t end = len - 1;
  *trailing_slash = raw[end - 1] == '/';
  while (end && raw[end - 1] == '/') { --end; }
  if (!end) { return -EBUSY; }
  raw[end] = 0;
  const char *last = base_name(raw);
  if (str_eq(last, ".") || str_eq(last, "..")) { return -EBUSY; }
  int64_t rc = syscall_resolve_path_at(dirfd, raw, path, CELL_PATH_MAX);
  if (rc) { return rc; }
  const struct vfs_mount *mount;
  for (size_t i = 0; (mount = vfs_mount_at(i)); ++i) {
    if (str_eq(path, mount->target)) { return -EBUSY; }
  }
  return syscall_may_change_entry(path, true);
}

int64_t sys_renameat(uint64_t old_dirfd, uint64_t old_addr, uint64_t new_dirfd, uint64_t new_addr, uint64_t flags) {
  constexpr uint64_t RENAME_NOREPLACE = 1;
  if (flags & ~RENAME_NOREPLACE) { return -EINVAL; }
  char old_path[CELL_PATH_MAX], new_path[CELL_PATH_MAX];
  bool old_slash, new_slash;
  int64_t rc = rename_path(old_dirfd, old_addr, old_path, &old_slash);
  if (rc) { return rc; }
  rc = rename_path(new_dirfd, new_addr, new_path, &new_slash);
  if (rc) { return rc; }
  if (old_slash || new_slash) {
    struct vfs_node src;
    if (!vfs_lstat(old_path, &src)) { return -ENOENT; }
    if (!src.is_dir) { return -ENOTDIR; }
  }
  return vfs_rename(old_path, new_path, flags & RENAME_NOREPLACE);
}

int64_t sys_linkat(uint64_t old_dirfd, uint64_t old_path_addr, uint64_t new_dirfd, uint64_t new_path_addr,
                   uint64_t flags) {
  if ((flags & ~(uint64_t)(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)) != 0) { return -EINVAL; }
  char first;
  if (!syscall_user_readable(new_path_addr, 1) || !vmm_copy_from_user(syscall_active_as(), &first, new_path_addr, 1)) {
    return -EFAULT;
  }
  if (!first) { return -ENOENT; }
  char old_path[CELL_PATH_MAX], new_path[CELL_PATH_MAX];
  int64_t rc = syscall_copy_resolved_path_at(new_dirfd, new_path_addr, new_path, sizeof(new_path));
  if (rc != 0) { return rc; }
  rc = syscall_may_change_entry(new_path, false);
  if (rc) { return rc; }

  if (!syscall_user_readable(old_path_addr, 1) || !vmm_copy_from_user(syscall_active_as(), &first, old_path_addr, 1)) {
    return -EFAULT;
  }
  int fd = -1;
  if (first == 0) {
    if (!(flags & AT_EMPTY_PATH) || !cell_capable(CAP_DAC_READ_SEARCH)) { return -ENOENT; }
    fd = (int)old_dirfd;
  } else {
    rc = syscall_copy_resolved_path_at(old_dirfd, old_path_addr, old_path, sizeof(old_path));
    if (rc != 0) { return rc; }
    if (!(flags & AT_SYMLINK_FOLLOW) || !syscall_proc_fd_number(old_path, &fd)) {
      return vfs_link(old_path, new_path) ? 0 : -ENOENT;
    }
  }
  struct vfs_node src, parent;
  int open_flags = cell_fd_get_flags(fd);
  if (open_flags < 0 || !cell_fd_stat(fd, &src)) { return first == 0 ? -EBADF : -ENOENT; }
  if (src.is_dir) { return -EPERM; }
  if (!(open_flags & O_TMPFILE_BIT)) { return -ENOENT; }
  if (!cell_capable(CAP_FOWNER) && cell_current_euid() != src.uid) { return -EPERM; }
  if (vfs_lstat(new_path, &parent)) { return -EEXIST; }
  size_t component = (size_t)(base_name(new_path) - new_path);
  size_t split = component > 1 ? component - 1 : 1;
  char saved = new_path[split];
  new_path[split] = 0;
  bool found = vfs_lookup(new_path, &parent);
  new_path[split] = saved;
  if (!found) { return -ENOENT; }
  if (!parent.is_dir) { return -ENOTDIR; }
  if (src.dev_id != parent.dev_id) { return -EXDEV; }
  if (!syscall_node_access_allowed(&parent, W_OK | X_OK)) { return -EACCES; }
  return vfs_link_node(&src, new_path) ? 0 : -ENOENT;
}

int64_t sys_symlinkat(uint64_t target_addr, uint64_t new_dirfd, uint64_t link_path_addr) {
  char target[CELL_PATH_MAX];
  char link_path[CELL_PATH_MAX];
  if (!syscall_copy_string_from_user(target_addr, target, sizeof(target))) {
    return -EFAULT;
  }
  int64_t path_rc = syscall_copy_resolved_path_at(new_dirfd, link_path_addr, link_path, sizeof(link_path));
  if (path_rc != 0) { return path_rc; }
  path_rc = syscall_may_change_entry(link_path, false);
  if (path_rc) { return path_rc; }
  if (!vfs_symlink(target, link_path)) { return -EEXIST; }
  struct vfs_node node;
  if (!vfs_lstat(link_path, &node) || !vfs_chown_node(&node, cell_current_euid(), cell_current_egid())) { return -EIO; }
  return 0;
}

int64_t sys_readlinkat(uint64_t dirfd, uint64_t path_addr, uint64_t buf, uint64_t len) {
  if (!path_addr) { return -EFAULT; }
  if (!len) { return -EINVAL; }
  struct vfs_node node;
  int rc = syscall_lookup_at(dirfd, path_addr, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, 0, &node, NULL);
  if (rc) { return rc; }
  char target[FD_PATH_MAX];
  size_t target_len;
  if (!vfs_readlink(&node, target, sizeof(target), &target_len)) { return -EINVAL; }
  if (target_len > len) { target_len = len; }
  if (!syscall_user_writable(buf, target_len)) { return -EFAULT; }
  return vmm_copy_to_user(syscall_active_as(), buf, target, target_len) ? (int64_t)target_len : -EFAULT;
}

int64_t sys_fchdir(uint64_t fd) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  if (!node.is_dir) { return -(int64_t)ENOTDIR; }
  if (!syscall_node_access_allowed(&node, 1)) { return -EACCES; }
  char path[CELL_PATH_MAX];
  if (!cell_fd_path((int)fd, path, sizeof(path))) { return -(int64_t)EBADF; }
  return cell_set_cwd(path) ? 0 : -(int64_t)ENAMETOOLONG;
}
