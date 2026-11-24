#include "abi/linux/aarch64/layout.h"
#include "handlers.h"
#include "proc/accounting.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/policy.h"
#include "fs/vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  AT_FDCWD = -100,
  AT_EMPTY_PATH = 0x1000,
  AT_SYMLINK_NOFOLLOW = 0x100,
  AT_SYMLINK_FOLLOW = 0x400,
  AT_EACCESS = 0x200,
  X_OK = 1,
  W_OK = 2,
  R_OK = 4,
  O_ACCMODE = 3,
  O_WRONLY = 1,
  O_RDWR = 2,
  O_CREAT = 0100,
  O_EXCL = 0200,
  O_TRUNC = 01000,
  O_DIRECTORY = 040000,
  O_TMPFILE_BIT = 020000000,
  O_CLOEXEC = 02000000,
  MFD_CLOEXEC = 1,
  MFD_ALLOW_SEALING = 2,
  S_IFMT = 0170000,
  S_IFIFO = 0010000,
  EXDEV = 18,
  ENOENT = 2,
  EPERM = 1,
  EBADF = 9,
  EACCES = 13,
  EFAULT = 14,
  EEXIST = 17,
  EINVAL = 22,
  ENOTDIR = 20,
  EISDIR = 21,
  ENAMETOOLONG = 36,
  ENOTEMPTY = 39,
  EIO = 5,
  EBUSY = 16,
  E2BIG = 7,
  STATX_BASIC_STATS = 0x000007ff,
};

static uint64_t memfd_sequence;

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
  uint32_t stx_dio_mem_align;
  uint32_t stx_dio_offset_align;
  uint64_t __spare3[12];
};

static_assert(sizeof(struct statx64) == 256, "Linux statx ABI size");

static bool proc_self_fd_number(const char *path, int *fd_out) {
  const char *prefix = "/proc/self/fd/";
  if (!starts_with(path, prefix)) { return false; }
  const char *p = path + kstrlen(prefix);
  if (*p == '\0') { return false; }
  int fd = 0;
  while (*p != '\0') {
    if (*p < '0' || *p > '9') { return false; }
    if (fd > (INT32_MAX - (*p - '0')) / 10) { return false; }
    fd = fd * 10 + (*p - '0');
    ++p;
  }
  *fd_out = fd;
  return true;
}

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

int64_t sys_openat(uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mode) {
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
  if ((flags & O_TMPFILE_BIT) != 0) {
    if ((flags & (O_CREAT | O_DIRECTORY)) != O_DIRECTORY ||
        ((flags & O_ACCMODE) != O_RDWR && (flags & O_ACCMODE) != O_WRONLY)) {
      return -EINVAL;
    }
    char first;
    if (!vmm_copy_from_user(syscall_active_as(), &first, path_addr, 1)) { return -EFAULT; }
    if (!first) { return -ENOENT; }
    struct vfs_node dir, tmp;
    if (!vfs_lookup(path, &dir)) { return -ENOENT; }
    if (!dir.is_dir) { return -ENOTDIR; }
    if (!syscall_node_access_allowed(&dir, W_OK | X_OK)) { return -EACCES; }
    int rc = vfs_tmpfile(&dir, mode & 07777u & ~SYSCALL_UMASK, cell_current_euid(), cell_current_egid(),
                         (flags & O_EXCL) == 0, &tmp);
    if (rc < 0) { return rc; }
    size_t len = kstrlen(virtual_path);
    len = append_literal(virtual_path, sizeof(virtual_path), len, len == 1 ? "#" : "/#");
    len = append_u64(virtual_path, sizeof(virtual_path), len, tmp.ino);
    append_literal(virtual_path, sizeof(virtual_path), len, " (deleted)");
    int fd = cell_fd_open_node(&tmp, flags, virtual_path);
    vfs_release_node(&tmp);
    return fd;
  }
  int proc_fd = -1;
  if (proc_self_fd_number(path, &proc_fd)) {
    int dup = cell_fd_dup(proc_fd, 0);
    if (dup < 0) { return dup; }
    if ((flags & O_CLOEXEC) != 0) {
      int rc = cell_fd_set_fd_flags(dup, 1);
      if (rc < 0) {
        (void)cell_fd_close(dup);
        return rc;
      }
    }
    return dup;
  }
  struct vfs_node node;
  bool exists = vfs_lookup(path, &node);
  if (exists && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) { return -(int64_t)EEXIST; }
  if (!exists) {
    uint16_t create_mode = (uint16_t)(mode & 07777u & ~SYSCALL_UMASK);
    if ((flags & O_CREAT) == 0 || !vfs_create(path, create_mode, &node)) { return -(int64_t)ENOENT; }
    (void)vfs_chown_node(&node, cell_current_euid(), cell_current_egid());
    (void)vfs_lookup(path, &node);
  } else if (!syscall_node_access_allowed(&node, access)) {
    return -(int64_t)EACCES;
  }
  if ((flags & O_TRUNC) != 0 && !node.is_dir) {
    (void)vfs_truncate(&node, 0);
    (void)vfs_lookup(path, &node);
  }
  return cell_fd_open_node(&node, (uint32_t)flags, virtual_path);
}

static bool memfd_name_char_ok(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
         ch == '-';
}

int64_t sys_memfd_create(uint64_t name_addr, uint64_t flags) {
  if ((flags & ~(uint64_t)(MFD_CLOEXEC | MFD_ALLOW_SEALING)) != 0) { return -(int64_t)EINVAL; }
  char name[64];
  if (!syscall_copy_string_from_user(name_addr, name, sizeof(name))) { return -(int64_t)EFAULT; }
  for (size_t i = 0; name[i] != '\0'; ++i) {
    if (!memfd_name_char_ok(name[i])) { name[i] = '_'; }
  }

  for (unsigned attempt = 0; attempt < 16; ++attempt) {
    char path[CELL_PATH_MAX];
    size_t len = append_literal(path, sizeof(path), 0, "/run/memfd/");
    len = append_u64(path, sizeof(path), len, (uint64_t)cell_current_pid());
    len = append_literal(path, sizeof(path), len, "-");
    len = append_u64(path, sizeof(path), len, ++memfd_sequence);
    len = append_literal(path, sizeof(path), len, "-");
    len = append_literal(path, sizeof(path), len, name[0] == '\0' ? "memfd" : name);
    if (len + 1 >= sizeof(path)) { return -(int64_t)ENAMETOOLONG; }
    struct vfs_node node;
    if (!vfs_create(path, 0666, &node)) { continue; }
    (void)vfs_chown_node(&node, cell_current_euid(), cell_current_egid());
    (void)vfs_lookup(path, &node);
    (void)vfs_set_sealable(&node, (flags & MFD_ALLOW_SEALING) != 0);
    uint32_t open_flags = O_RDWR | ((flags & MFD_CLOEXEC) != 0 ? O_CLOEXEC : 0);
    int fd = cell_fd_open_node(&node, open_flags, path);
    if (fd >= 0) { (void)vfs_remove(path); }
    return fd;
  }
  return -(int64_t)EEXIST;
}

static void fill_stat(struct stat64_aarch64 *st, const struct vfs_node *node) {
  kmemset(st, 0, sizeof(*st));
  st->st_dev = node->dev_id;
  st->st_ino = node->ino;
  st->st_mode = node->mode;
  st->st_nlink = node->links_count;
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

static bool fill_statfs(struct statfs64_aarch64 *st, const struct vfs_node *node) {
  struct vfs_fs_info info;
  if (!vfs_device_fs_info(node->dev_id, &info)) { return false; }
  kmemset(st, 0, sizeof(*st));
  st->f_type = info.magic;
  st->f_bsize = info.block_size;
  st->f_blocks = info.block_count;
  st->f_bfree = info.free_blocks;
  st->f_bavail = info.free_blocks;
  st->f_files = info.inode_count;
  st->f_ffree = info.free_inodes;
  st->f_namelen = 255;
  st->f_frsize = st->f_bsize;
  return true;
}

static void fill_statx(struct statx64 *st, const struct vfs_node *node) {
  kmemset(st, 0, sizeof(*st));
  st->stx_mask = STATX_BASIC_STATS;
  st->stx_blksize = PAGE_SIZE;
  st->stx_nlink = node->links_count;
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
  if (!fill_statfs(&st, &node)) { return -(int64_t)EIO; }
  return syscall_user_writable(statfs_addr, sizeof(st)) &&
             vmm_copy_to_user(syscall_active_as(), statfs_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_fstatfs(uint64_t fd, uint64_t statfs_addr) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  struct statfs64_aarch64 st;
  if (!fill_statfs(&st, &node)) { return -(int64_t)EIO; }
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
  if (!vfs_mkdir(path, (uint32_t)mode)) {
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
  if (!vfs_mkfifo(path, (uint32_t)mode, &node)) { return -(int64_t)EEXIST; }
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
  return vfs_remove(path);
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
  if ((flags & ~(uint64_t)(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)) != 0) { return -EINVAL; }
  char first;
  if (!syscall_user_readable(new_path_addr, 1) || !vmm_copy_from_user(syscall_active_as(), &first, new_path_addr, 1)) {
    return -EFAULT;
  }
  if (!first) { return -ENOENT; }
  char old_path[CELL_PATH_MAX], new_path[CELL_PATH_MAX];
  int64_t rc = syscall_copy_resolved_path_at(new_dirfd, new_path_addr, new_path, sizeof(new_path));
  if (rc != 0) { return rc; }
  if (!cell_fs_path_allowed(new_path, syscall_fs_rights_from_access(W_OK))) { return -EPERM; }

  if (!syscall_user_readable(old_path_addr, 1) || !vmm_copy_from_user(syscall_active_as(), &first, old_path_addr, 1)) {
    return -EFAULT;
  }
  int fd = -1;
  if (first == 0) {
    if (!(flags & AT_EMPTY_PATH) || cell_current_euid() != 0) { return -ENOENT; }
    fd = (int)old_dirfd;
  } else {
    rc = syscall_copy_resolved_path_at(old_dirfd, old_path_addr, old_path, sizeof(old_path));
    if (rc != 0) { return rc; }
    if (!(flags & AT_SYMLINK_FOLLOW) || !proc_self_fd_number(old_path, &fd)) {
      return vfs_link(old_path, new_path) ? 0 : -ENOENT;
    }
  }
  struct vfs_node src, parent;
  int open_flags = cell_fd_get_flags(fd);
  if (open_flags < 0 || !cell_fd_stat(fd, &src)) { return first == 0 ? -EBADF : -ENOENT; }
  if (src.is_dir) { return -EPERM; }
  if (!(open_flags & O_TMPFILE_BIT)) { return -ENOENT; }
  if (cell_current_euid() != 0 && cell_current_euid() != src.uid) { return -EPERM; }
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
  if (vfs_truncate_sealed(&node, size)) { return -(int64_t)EPERM; }
  return vfs_truncate(&node, size) ? 0 : -(int64_t)EINVAL;
}
