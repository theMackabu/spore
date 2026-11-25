#include "proc/io.h"
#include "../usercopy.h"
#include "file_abi.h"
#include "abi/linux/shared/layout.h"
#include "abi/linux/shared/open.h"
#include "fs/notify.h"
#include "fs/vfs.h"
#include "proc/accounting.h"
#include "proc/domain.h"
#include "proc/capabilities.h"
#include "proc/fd.h"
#include "proc/policy.h"

struct attr_target {
  struct vfs_node node;
  struct open_file *file;
  char path[CELL_PATH_MAX];
};

static int attr_target(uint64_t fd, uint64_t addr, uint64_t flags, bool descriptor, struct attr_target *target) {
  target->file = NULL;
  if (flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) { return -EINVAL; }
  if (descriptor) {
    if (flags) { return -EINVAL; }
    struct open_file *file = cell_fd_file(fd);
    if (!file || (file->flags & LINUX_O_PATH)) { return -EBADF; }
    if (!cell_fd_stat((int)fd, &target->node)) { return -EBADF; }
    target->file = file;
    return vfs_readonly_node(&target->node) ? -30 : 0;
  }
  int rc = syscall_lookup_at(fd, addr, flags, CELL_FS_WRITE, &target->node, target->path);
  if (!rc && vfs_readonly_node(&target->node)) { return -30; }
  if (!rc && !target->path[0]) { target->file = cell_fd_file(fd); }
  return rc;
}

static void notify_attr(const struct attr_target *target, uint32_t mask) {
  if (target->file) { fsnotify_file_event(target->file, mask); }
  else { fsnotify_node_event(&target->node, target->path, mask); }
}

int64_t sys_utimensat(uint64_t dirfd, uint64_t path_addr, uint64_t times_addr, uint64_t flags) {
  constexpr int64_t NOW = (1 << 30) - 1;
  constexpr int64_t OMIT = NOW - 1;
  struct timespec64 times[2] = {{.tv_nsec = NOW}, {.tv_nsec = NOW}};
  if (times_addr && (!syscall_user_readable(times_addr, sizeof(times)) ||
      !vmm_copy_from_user(syscall_active_as(), times, times_addr, sizeof(times)))) { return -EFAULT; }
  for (size_t i = 0; i < 2; ++i) {
    int64_t ns = times[i].tv_nsec;
    if (ns != NOW && ns != OMIT && (ns < 0 || ns >= 1000000000)) { return -EINVAL; }
  }
  if (times[0].tv_nsec == OMIT && times[1].tv_nsec == OMIT) { return 0; }
  struct attr_target target;
  int rc = attr_target(dirfd, path_addr, flags, !path_addr, &target);
  if (rc) { return rc; }
  if (!cell_capable(CAP_FOWNER) && cell_current_euid() != target.node.uid) {
    if (times[0].tv_nsec != NOW || times[1].tv_nsec != NOW) { return -EPERM; }
    if (!syscall_node_access_allowed(&target.node, W_OK)) { return -EACCES; }
  }
  int64_t now = (int64_t)cell_realtime_seconds();
  int64_t atime = times[0].tv_nsec == OMIT ? (int64_t)target.node.atime : times[0].tv_nsec == NOW ? now : times[0].tv_sec;
  int64_t mtime = times[1].tv_nsec == OMIT ? (int64_t)target.node.mtime : times[1].tv_nsec == NOW ? now : times[1].tv_sec;
  rc = vfs_set_times(&target.node, atime, mtime);
  if (!rc) { notify_attr(&target, times[0].tv_nsec == OMIT ? FS_MODIFY : times[1].tv_nsec == OMIT ? FS_ACCESS : FS_ATTRIB); }
  return rc;
}

static int chmod_target(uint64_t fd, uint64_t addr, uint64_t flags, bool descriptor, uint32_t mode) {
  struct attr_target target;
  int rc = attr_target(fd, addr, flags, descriptor, &target);
  if (rc) { return rc; }
  if ((target.node.mode & S_IFMT) == S_IFLNK) { return -95; }
  if (cell_current_euid() != target.node.uid && !cell_capable(CAP_FOWNER)) { return -EPERM; }
  if (!cell_in_group(target.node.gid) && !cell_capable(CAP_FSETID)) { mode &= ~02000u; }
  if (!vfs_chmod_node(&target.node, mode)) { return -EINVAL; }
  notify_attr(&target, FS_ATTRIB);
  return 0;
}

int64_t sys_fchmodat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t flags) {
  return chmod_target(dirfd, path_addr, flags, false, mode);
}

int64_t sys_fchmod(uint64_t fd, uint64_t mode) {
  return chmod_target(fd, 0, 0, true, mode);
}

static bool chown_ids(const struct vfs_node *node, uint64_t uid_arg, uint64_t gid_arg, uint32_t *uid, uint32_t *gid) {
  *uid = uid_arg == 0xffffffffull ? node->uid : (uint32_t)uid_arg;
  *gid = gid_arg == 0xffffffffull ? node->gid : (uint32_t)gid_arg;
  return uid_arg <= 0xffffffffull && gid_arg <= 0xffffffffull;
}

static int chown_target(uint64_t fd, uint64_t addr, uint64_t flags, bool descriptor, uint64_t uid_arg, uint64_t gid_arg) {
  struct attr_target target;
  int rc = attr_target(fd, addr, flags, descriptor, &target);
  if (rc) { return rc; }
  if (!cell_capable(CAP_CHOWN)) { return -EPERM; }
  uint32_t uid, gid;
  if (!chown_ids(&target.node, uid_arg, gid_arg, &uid, &gid)) { return -EINVAL; }
  if (!vfs_chown_node(&target.node, uid, gid)) { return -EINVAL; }
  notify_attr(&target, FS_ATTRIB);
  return 0;
}

int64_t sys_fchownat(uint64_t dirfd, uint64_t path_addr, uint64_t uid, uint64_t gid, uint64_t flags) {
  return chown_target(dirfd, path_addr, flags, false, uid, gid);
}

int64_t sys_fchown(uint64_t fd, uint64_t uid, uint64_t gid) {
  return chown_target(fd, 0, 0, true, uid, gid);
}

int64_t sys_ftruncate(uint64_t fd, uint64_t size) {
  struct open_file *file = cell_fd_file(fd);
  if (!file || (file->flags & LINUX_O_PATH)) { return -EBADF; }
  if (file->type != OPEN_VFS || !linux_open_writable(file->flags) || (int64_t)size < 0 ||
      file->node.is_dir || file->node.device != RAMFS_DEV_NONE) { return -EINVAL; }
  if (vfs_truncate_sealed(&file->node, size)) { return -EPERM; }
  struct domain *d=cell_current_domain_internal();
  if (size>d->fsize_soft) {
    struct vfs_node fresh;
    if (!vfs_refresh(&file->node,&fresh)) {return -5;}
    if (size>fresh.size) {size_t bytes=1;return cell_file_write_limit(d,size,&bytes);}
  }
  if (!vfs_truncate(&file->node, size)) { return -EINVAL; }
  fsnotify_file_event(file, FS_MODIFY);
  return 0;
}

int64_t sys_fsync(uint64_t fd) {
  struct open_file *file = cell_fd_file(fd);
  if (!file) { return -EBADF; }
  if (file->type == OPEN_VFS && !vfs_sync_mapped_range(&file->node, 0, UINT64_MAX)) { return -EIO; }
  return vfs_sync_storage() ? 0 : -EIO;
}

int64_t sys_faccessat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t flags) {
  if ((mode & ~(uint64_t)(R_OK | W_OK | X_OK)) != 0) { return -EINVAL; }
  if ((flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_EMPTY_PATH)) != 0) { return -EINVAL; }
  struct vfs_node node;
  int rc = syscall_lookup_at(dirfd, path_addr, flags, syscall_fs_rights_from_access(mode), &node, NULL);
  if (rc) { return rc; }
  return syscall_node_access_allowed(&node, mode) ? 0 : -EACCES;
}
