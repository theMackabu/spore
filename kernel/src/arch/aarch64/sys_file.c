#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mem.h"
#include "mm/pmm.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  AT_FDCWD = -100,
  AT_EMPTY_PATH = 0x1000,
  AT_SYMLINK_NOFOLLOW = 0x100,
  AT_EACCESS = 0x200,
  F_OK = 0,
  X_OK = 1,
  W_OK = 2,
  R_OK = 4,
  O_ACCMODE = 3,
  O_WRONLY = 1,
  O_RDWR = 2,
  O_CREAT = 0100,
  O_EXCL = 0200,
  O_TRUNC = 01000,
  S_IFMT = 0170000,
  S_IFIFO = 0010000,
  DT_REG = 8,
  DT_DIR = 4,
  DT_CHR = 2,
  EROFS = 30,
  ENOENT = 2,
  EPERM = 1,
  EBADF = 9,
  EACCES = 13,
  EFAULT = 14,
  EEXIST = 17,
  EINVAL = 22,
  ENOTDIR = 20,
  ENAMETOOLONG = 36,
  ENOTEMPTY = 39,
  EXT2_SUPER_MAGIC = 0xef53,
  TMPFS_MAGIC = 0x01021994,
  PROC_SUPER_MAGIC = 0x9fa0,
  DEVFS_SUPER_MAGIC = 0x1373,
  STATX_BASIC_STATS = 0x000007ff,
};

struct stat64_aarch64 {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  int64_t st_atime_sec;
  int64_t st_atime_nsec;
  int64_t st_mtime_sec;
  int64_t st_mtime_nsec;
  int64_t st_ctime_sec;
  int64_t st_ctime_nsec;
  uint32_t __unused[2];
};

struct statfs64_aarch64 {
  uint64_t f_type;
  uint64_t f_bsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  int32_t f_fsid[2];
  uint64_t f_namelen;
  uint64_t f_frsize;
  uint64_t f_flags;
  uint64_t f_spare[4];
};

struct statx_timestamp64 {
  int64_t tv_sec;
  uint32_t tv_nsec;
  int32_t __reserved;
};

struct statx64 {
  uint32_t stx_mask;
  uint32_t stx_blksize;
  uint64_t stx_attributes;
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode;
  uint16_t __spare0;
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks;
  uint64_t stx_attributes_mask;
  struct statx_timestamp64 stx_atime;
  struct statx_timestamp64 stx_btime;
  struct statx_timestamp64 stx_ctime;
  struct statx_timestamp64 stx_mtime;
  uint32_t stx_rdev_major;
  uint32_t stx_rdev_minor;
  uint32_t stx_dev_major;
  uint32_t stx_dev_minor;
  uint64_t stx_mnt_id;
  uint64_t stx_dio_mem_align;
  uint64_t stx_dio_offset_align;
  uint64_t __spare3[12];
};

struct linux_dirent64_header {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
} __attribute__((packed));

int64_t sys_openat(uint64_t dirfd, uint64_t path_addr, uint64_t flags) {
  char path[CELL_PATH_MAX];
  char virtual_path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  path_rc = syscall_copy_virtual_path_at(dirfd, path_addr, virtual_path, sizeof(virtual_path));
  if (path_rc != 0) { return path_rc; }
  uint64_t access = 0;
  if ((flags & O_ACCMODE) == O_WRONLY) {
    access = W_OK;
  } else if ((flags & O_ACCMODE) == O_RDWR) {
    access = R_OK | W_OK;
  } else {
    access = R_OK;
  }
  if ((flags & O_TRUNC) != 0) { access |= W_OK; }
  if (!cell_fs_path_allowed(path, syscall_fs_rights_from_access(access))) { return -(int64_t)EPERM; }
  struct vfs_node node;
  bool exists = vfs_lookup(path, &node);
  if (exists && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) { return -(int64_t)EEXIST; }
  if (!exists) {
    if ((flags & O_CREAT) == 0 || !vfs_create(path, &node)) { return -(int64_t)ENOENT; }
    (void)vfs_chown_node(&node, cell_current_euid(), cell_current_egid());
    (void)vfs_lookup(path, &node);
  }
  if (!syscall_node_access_allowed(&node, access)) { return -(int64_t)EACCES; }
  if ((flags & O_TRUNC) != 0 && !node.is_dir) {
    (void)vfs_truncate(&node, 0);
    (void)vfs_lookup(path, &node);
  }
  return cell_fd_open_node(&node, (uint32_t)flags, virtual_path);
}

static void fill_stat(struct stat64_aarch64 *st, const struct vfs_node *node) {
  kmemset(st, 0, sizeof(*st));
  st->st_dev = node->dev_id;
  st->st_ino = node->ino;
  st->st_mode = node->mode;
  st->st_nlink = node->links_count == 0 ? 1 : node->links_count;
  st->st_uid = node->uid;
  st->st_gid = node->gid;
  st->st_rdev = node->rdev;
  st->st_size = (int64_t)node->size;
  st->st_blksize = PAGE_SIZE;
  st->st_blocks = (int64_t)((node->size + 511) / 512);
  uint64_t now = cell_realtime_seconds();
  uint64_t atime = node->atime == 0 ? now : node->atime;
  uint64_t mtime = node->mtime == 0 ? now : node->mtime;
  uint64_t ctime = node->ctime == 0 ? now : node->ctime;
  st->st_atime_sec = (int64_t)atime;
  st->st_mtime_sec = (int64_t)mtime;
  st->st_ctime_sec = (int64_t)ctime;
}

static uint32_t dev_major(uint64_t dev) {
  return (uint32_t)(dev >> 8);
}

static uint32_t dev_minor(uint64_t dev) {
  return (uint32_t)(dev & 0xffu);
}

static uint64_t statfs_magic_for_path(const char *path) {
  if (path[0] == '/' && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' && path[4] == 'c' &&
      (path[5] == '\0' || path[5] == '/')) {
    return PROC_SUPER_MAGIC;
  }
  if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && (path[4] == '\0' || path[4] == '/')) {
    return DEVFS_SUPER_MAGIC;
  }
  if (path[0] == '/' && path[1] == 's' && path[2] == 'y' && path[3] == 's' && (path[4] == '\0' || path[4] == '/')) {
    return 0x62656572u;
  }
  if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' && (path[4] == '\0' || path[4] == '/')) {
    return TMPFS_MAGIC;
  }
  return EXT2_SUPER_MAGIC;
}

static void fill_statfs(struct statfs64_aarch64 *st, const char *path) {
  struct vfs_fs_info info;
  kmemset(st, 0, sizeof(*st));
  if (!vfs_fs_info(&info)) {
    info.block_size = PAGE_SIZE;
    info.block_count = 0;
    info.free_blocks = 0;
    info.inode_count = 0;
    info.free_inodes = 0;
  }
  st->f_type = statfs_magic_for_path(path);
  st->f_bsize = info.block_size == 0 ? PAGE_SIZE : info.block_size;
  st->f_blocks = info.block_count;
  st->f_bfree = info.free_blocks;
  st->f_bavail = info.free_blocks;
  st->f_files = info.inode_count;
  st->f_ffree = info.free_inodes;
  st->f_namelen = 255;
  st->f_frsize = st->f_bsize;
}

static void fill_statx(struct statx64 *st, const struct vfs_node *node) {
  kmemset(st, 0, sizeof(*st));
  st->stx_mask = STATX_BASIC_STATS;
  st->stx_blksize = PAGE_SIZE;
  st->stx_nlink = node->links_count == 0 ? 1 : node->links_count;
  st->stx_uid = node->uid;
  st->stx_gid = node->gid;
  st->stx_mode = node->mode;
  st->stx_ino = node->ino;
  st->stx_size = node->size;
  st->stx_blocks = (node->size + 511) / 512;
  st->stx_rdev_major = dev_major(node->rdev);
  st->stx_rdev_minor = dev_minor(node->rdev);
  st->stx_dev_major = dev_major(node->dev_id);
  st->stx_dev_minor = dev_minor(node->dev_id);
  uint64_t now = cell_realtime_seconds();
  uint64_t atime = node->atime == 0 ? now : node->atime;
  uint64_t mtime = node->mtime == 0 ? now : node->mtime;
  uint64_t ctime = node->ctime == 0 ? now : node->ctime;
  st->stx_atime.tv_sec = (int64_t)atime;
  st->stx_mtime.tv_sec = (int64_t)mtime;
  st->stx_ctime.tv_sec = (int64_t)ctime;
  st->stx_btime.tv_sec = (int64_t)ctime;
}

int64_t sys_fstat(uint64_t fd, uint64_t stat_addr) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  struct stat64_aarch64 st;
  fill_stat(&st, &node);
  return syscall_user_writable(stat_addr, sizeof(st)) &&
             vmm_copy_to_user(syscall_active_as(), stat_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_statfs(uint64_t path_addr, uint64_t statfs_addr) {
  char path[CELL_PATH_MAX];
  if (!syscall_copy_resolved_path(path_addr, path, sizeof(path))) {
    return syscall_path_policy_denied() ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  struct vfs_node node;
  if (!vfs_lookup(path, &node)) { return -(int64_t)ENOENT; }
  struct statfs64_aarch64 st;
  fill_statfs(&st, path);
  return syscall_user_writable(statfs_addr, sizeof(st)) &&
             vmm_copy_to_user(syscall_active_as(), statfs_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_fstatfs(uint64_t fd, uint64_t statfs_addr) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  struct statfs64_aarch64 st;
  fill_statfs(&st, node.backend == VFS_RAMFS ? "/tmp" : "/");
  return syscall_user_writable(statfs_addr, sizeof(st)) &&
             vmm_copy_to_user(syscall_active_as(), statfs_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_statx(uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mask, uint64_t statx_addr) {
  (void)mask;
  struct vfs_node node;
  if ((flags & AT_EMPTY_PATH) != 0 && path_addr != 0) {
    char empty[1];
    if (!vmm_copy_from_user(syscall_active_as(), empty, path_addr, 1)) { return -(int64_t)EFAULT; }
    if (empty[0] == '\0') {
      if ((int64_t)dirfd == AT_FDCWD) {
        char cwd[CELL_PATH_MAX];
        if (!syscall_normalize_path("/", cell_current_cwd(), cwd, sizeof(cwd)) || !vfs_lookup(cwd, &node)) {
          return -(int64_t)ENOENT;
        }
      } else if (!cell_fd_stat((int)dirfd, &node)) {
        return -(int64_t)EBADF;
      }
      struct statx64 st;
      fill_statx(&st, &node);
      return syscall_user_writable(statx_addr, sizeof(st)) &&
                 vmm_copy_to_user(syscall_active_as(), statx_addr, &st, sizeof(st))
               ? 0
               : -(int64_t)EFAULT;
    }
  }
  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  bool nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
  if (!(nofollow ? vfs_lstat(path, &node) : vfs_lookup(path, &node))) { return -(int64_t)ENOENT; }
  struct statx64 st;
  fill_statx(&st, &node);
  return syscall_user_writable(statx_addr, sizeof(st)) &&
             vmm_copy_to_user(syscall_active_as(), statx_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr, uint64_t stat_addr, uint64_t flags) {
  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  struct vfs_node node;
  bool nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
  if (!(nofollow ? vfs_lstat(path, &node) : vfs_lookup(path, &node))) { return -(int64_t)ENOENT; }
  struct stat64_aarch64 st;
  fill_stat(&st, &node);
  return syscall_user_writable(stat_addr, sizeof(st)) &&
             vmm_copy_to_user(syscall_active_as(), stat_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_faccessat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t flags) {
  if ((mode & ~(uint64_t)(R_OK | W_OK | X_OK)) != 0) { return -(int64_t)EINVAL; }
  if ((flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_EMPTY_PATH)) != 0) { return -(int64_t)EINVAL; }
  struct vfs_node node;
  if ((flags & AT_EMPTY_PATH) != 0 && path_addr != 0) {
    char empty[1];
    if (!vmm_copy_from_user(syscall_active_as(), empty, path_addr, 1)) { return -(int64_t)EFAULT; }
    if (empty[0] == '\0') {
      if ((int64_t)dirfd == AT_FDCWD) {
        char cwd[CELL_PATH_MAX];
        if (!syscall_normalize_path("/", cell_current_cwd(), cwd, sizeof(cwd)) || !vfs_lookup(cwd, &node)) {
          return -(int64_t)ENOENT;
        }
      } else if (!cell_fd_stat((int)dirfd, &node)) {
        return -(int64_t)EBADF;
      }
      return syscall_node_access_allowed(&node, mode) ? 0 : -(int64_t)EACCES;
    }
  }
  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  bool nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
  if (!(nofollow ? vfs_lstat(path, &node) : vfs_lookup(path, &node))) { return -(int64_t)ENOENT; }
  if (!cell_fs_path_allowed(path, syscall_fs_rights_from_access(mode))) { return -(int64_t)EPERM; }
  return syscall_node_access_allowed(&node, mode) ? 0 : -(int64_t)EACCES;
}

static uint16_t dirent_reclen(size_t name_len) {
  return (uint16_t)((sizeof(struct linux_dirent64_header) + name_len + 1 + 7) & ~7ull);
}

int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len) {
  uint64_t written = 0;
  for (;;) {
    struct vfs_dirent ent;
    uint64_t old_dir_offset = cell_fd_dir_offset((int)fd);
    if (!cell_fd_next_dirent((int)fd, &ent)) { break; }
    size_t name_len = kstrlen(ent.name);
    uint16_t reclen = dirent_reclen(name_len);
    if (written + reclen > len) {
      cell_fd_set_dir_offset((int)fd, old_dir_offset);
      break;
    }
    if (!syscall_user_writable(buf + written, reclen)) { return -(int64_t)EFAULT; }
    struct linux_dirent64_header hdr = {
      .d_ino = ent.ino,
      .d_off = (int64_t)cell_fd_dir_offset((int)fd),
      .d_reclen = reclen,
      .d_type = ent.type != 0 ? ent.type : (ent.is_dir ? DT_DIR : (ent.is_device ? DT_CHR : DT_REG)),
    };
    if (!vmm_copy_to_user(syscall_active_as(), buf + written, &hdr, sizeof(hdr)) ||
        !vmm_copy_to_user(syscall_active_as(), buf + written + sizeof(hdr), ent.name, name_len + 1)) {
      return -(int64_t)EFAULT;
    }
    written += reclen;
  }
  return (int64_t)written;
}

int64_t sys_getcwd(uint64_t buf, uint64_t len) {
  const char *cwd = cell_current_cwd();
  size_t need = kstrlen(cwd) + 1;
  if (len < need || !syscall_user_writable(buf, need)) { return -(int64_t)EFAULT; }
  return vmm_copy_to_user(syscall_active_as(), buf, cwd, need) ? (int64_t)buf : -(int64_t)EFAULT;
}

int64_t sys_chdir(uint64_t path_addr) {
  char virtual_path[CELL_PATH_MAX];
  char path[CELL_PATH_MAX];
  if (!syscall_copy_virtual_path(path_addr, virtual_path, sizeof(virtual_path)) ||
      !syscall_copy_resolved_path(path_addr, path, sizeof(path))) {
    return syscall_path_policy_denied() ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  struct vfs_node node;
  if (!vfs_lookup(path, &node)) { return -(int64_t)ENOENT; }
  if (!node.is_dir) { return -(int64_t)ENOTDIR; }
  return cell_set_cwd(virtual_path) ? 0 : -(int64_t)ENAMETOOLONG;
}

int64_t sys_chroot(uint64_t path_addr) {
  char path[CELL_PATH_MAX];
  if (!syscall_copy_resolved_path(path_addr, path, sizeof(path))) {
    return syscall_path_policy_denied() ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
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
  if (!vfs_mkdir(path, (uint32_t)mode)) { return -(int64_t)EINVAL; }
  (void)vfs_chown(path, cell_current_euid(), cell_current_egid());
  return 0;
}

int64_t sys_mknodat(uint64_t dirfd, uint64_t path_addr, uint64_t mode) {
  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  if ((mode & S_IFMT) != S_IFIFO) { return -(int64_t)EINVAL; }
  struct vfs_node node;
  if (!vfs_mkfifo(path, (uint32_t)mode, &node)) { return -(int64_t)EEXIST; }
  (void)vfs_chown_node(&node, cell_current_euid(), cell_current_egid());
  return 0;
}

int64_t sys_unlinkat(uint64_t dirfd, uint64_t path_addr) {
  char path[CELL_PATH_MAX];
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  struct vfs_node node;
  if (!vfs_lstat(path, &node)) { return -(int64_t)ENOENT; }
  return vfs_unlink(path) ? 0 : -(int64_t)ENOTEMPTY;
}

int64_t sys_renameat(uint64_t old_dirfd, uint64_t old_path_addr, uint64_t new_dirfd, uint64_t new_path_addr) {
  char old_path[CELL_PATH_MAX];
  char new_path[CELL_PATH_MAX];
  int64_t old_rc = syscall_copy_resolved_path_at(old_dirfd, old_path_addr, old_path, sizeof(old_path));
  if (old_rc != 0) { return old_rc; }
  int64_t new_rc = syscall_copy_resolved_path_at(new_dirfd, new_path_addr, new_path, sizeof(new_path));
  if (new_rc != 0) { return new_rc; }
  return vfs_rename(old_path, new_path) ? 0 : -(int64_t)ENOENT;
}

int64_t sys_linkat(uint64_t old_dirfd, uint64_t old_path_addr, uint64_t new_dirfd, uint64_t new_path_addr,
                   uint64_t flags) {
  if (flags != 0) { return -(int64_t)EINVAL; }
  char old_path[CELL_PATH_MAX];
  char new_path[CELL_PATH_MAX];
  int64_t old_rc = syscall_copy_resolved_path_at(old_dirfd, old_path_addr, old_path, sizeof(old_path));
  if (old_rc != 0) { return old_rc; }
  int64_t new_rc = syscall_copy_resolved_path_at(new_dirfd, new_path_addr, new_path, sizeof(new_path));
  if (new_rc != 0) { return new_rc; }
  return vfs_link(old_path, new_path) ? 0 : -(int64_t)ENOENT;
}

int64_t sys_symlinkat(uint64_t target_addr, uint64_t new_dirfd, uint64_t link_path_addr) {
  char target[CELL_PATH_MAX];
  char link_path[CELL_PATH_MAX];
  if (!syscall_copy_string_from_user(target_addr, target, sizeof(target))) {
    return syscall_path_policy_denied() ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  int64_t path_rc = syscall_copy_resolved_path_at(new_dirfd, link_path_addr, link_path, sizeof(link_path));
  if (path_rc != 0) { return path_rc; }
  return vfs_symlink(target, link_path) ? 0 : -(int64_t)EEXIST;
}

int64_t sys_readlinkat(uint64_t dirfd, uint64_t path_addr, uint64_t buf, uint64_t len) {
  char path[CELL_PATH_MAX];
  char target[CELL_PATH_MAX];
  size_t target_len = 0;
  int64_t path_rc = syscall_copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  if (len == 0 || !syscall_user_writable(buf, len)) { return -(int64_t)EFAULT; }
  if (!vfs_readlink(path, target, sizeof(target), &target_len)) { return -(int64_t)EINVAL; }
  if (target_len > len) { target_len = (size_t)len; }
  return vmm_copy_to_user(syscall_active_as(), buf, target, target_len) ? (int64_t)target_len : -(int64_t)EFAULT;
}

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
  return vfs_chmod_node(&node, (uint32_t)mode) ? 0 : -(int64_t)EINVAL;
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
  return vfs_chown_node(&node, uid, gid) ? 0 : -(int64_t)EINVAL;
}

int64_t sys_ftruncate(uint64_t fd, uint64_t size) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  return vfs_truncate(&node, size) ? 0 : -(int64_t)EINVAL;
}
