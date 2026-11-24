#include "handlers.h"
#include "abi/linux/shared/usercopy.h"
#include "abi/linux/shared/fs/file_abi.h"
#include "abi/linux/shared/file_layout.h"
#include "fs/vfs.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/fd.h"

struct linux_statfs64 {
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

static uint64_t allocated_sectors(const struct vfs_node *node) {
  if (node->backend == VFS_EXT2) { return node->ext2.sectors_count; }
  if (node->backend == VFS_RAMFS) { return (uint64_t)node->ramfs.allocated_pages * (RAMFS_PAGE_SIZE / 512); }
  return 0;
}

static void fill_stat(struct linux_stat64 *st, const struct vfs_node *node) {
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
  st->st_blocks = (int64_t)allocated_sectors(node);
  st->st_atime_sec = (int64_t)node->atime;
  st->st_mtime_sec = (int64_t)node->mtime;
  st->st_ctime_sec = (int64_t)node->ctime;
}

static uint32_t dev_major(uint64_t dev) {
  return (uint32_t)(dev >> 8);
}

static uint32_t dev_minor(uint64_t dev) {
  return (uint32_t)(dev & 0xffu);
}

static bool fill_statfs(struct linux_statfs64 *st, const struct vfs_node *node) {
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
  st->stx_blocks = allocated_sectors(node);
  st->stx_rdev_major = dev_major(node->rdev);
  st->stx_rdev_minor = dev_minor(node->rdev);
  st->stx_dev_major = dev_major(node->dev_id);
  st->stx_dev_minor = dev_minor(node->dev_id);
  st->stx_atime.tv_sec = (int64_t)node->atime;
  st->stx_mtime.tv_sec = (int64_t)node->mtime;
  st->stx_ctime.tv_sec = (int64_t)node->ctime;
}

static int copy_result(uint64_t addr, const void *value, size_t size) {
  if (!syscall_user_writable(addr, size) || !vmm_copy_to_user(syscall_active_as(), addr, value, size)) { return -EFAULT; }
  return 0;
}

static int copy_stat(uint64_t addr, const struct vfs_node *node) {
  struct linux_stat64 st;
  fill_stat(&st, node);
  return copy_result(addr, &st, sizeof(st));
}

static int copy_statfs(uint64_t addr, const struct vfs_node *node) {
  struct linux_statfs64 st;
  if (!fill_statfs(&st, node)) { return -EIO; }
  return copy_result(addr, &st, sizeof(st));
}

int64_t sys_fstat(uint64_t fd, uint64_t stat_addr) {
  struct vfs_node node;
  return cell_fd_stat((int)fd, &node) ? copy_stat(stat_addr, &node) : -EBADF;
}

int64_t sys_statfs(uint64_t path_addr, uint64_t statfs_addr) {
  char path[CELL_PATH_MAX];
  int64_t rc = syscall_copy_resolved_path_at(AT_FDCWD, path_addr, path, sizeof(path));
  if (rc) { return rc; }
  struct vfs_node node;
  return vfs_lookup(path, &node) ? copy_statfs(statfs_addr, &node) : -ENOENT;
}

int64_t sys_fstatfs(uint64_t fd, uint64_t statfs_addr) {
  struct vfs_node node;
  return cell_fd_stat((int)fd, &node) ? copy_statfs(statfs_addr, &node) : -EBADF;
}

int64_t sys_statx(uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mask, uint64_t statx_addr) {
  (void)mask;
  struct vfs_node node;
  int rc = syscall_lookup_at(dirfd, path_addr, flags, 0, &node, NULL);
  if (rc) { return rc; }
  struct statx64 st;
  fill_statx(&st, &node);
  return copy_result(statx_addr, &st, sizeof(st));
}

int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr, uint64_t stat_addr, uint64_t flags) {
  struct vfs_node node;
  int rc = syscall_lookup_at(dirfd, path_addr, flags, 0, &node, NULL);
  return rc ? rc : copy_stat(stat_addr, &node);
}
