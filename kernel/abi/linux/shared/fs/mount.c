#include "file_abi.h"
#include "../usercopy.h"
#include "fs/vfs.h"
#include "lib/mem.h"
#include "lib/kstr.h"
#include "fs/mount_namespace.h"
#include "proc/domain.h"
#include "proc/policy.h"

struct fs_info64 {
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
  uint64_t inode_count;
  uint64_t free_inodes;
};

int64_t sys_spore_fsinfo(uint64_t addr) {
  struct vfs_fs_info raw;
  if (!vfs_fs_info(&raw)) { return -(int64_t)EINVAL; }
  struct fs_info64 info = {
    .block_size = raw.block_size,
    .block_count = raw.block_count,
    .free_blocks = raw.free_blocks,
    .inode_count = raw.inode_count,
    .free_inodes = raw.free_inodes,
  };
  return syscall_user_writable(addr, sizeof(info)) && vmm_copy_to_user(syscall_active_as(), addr, &info, sizeof(info))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_spore_mountinfo(uint64_t addr, uint64_t bytes) {
  size_t max = addr ? bytes / sizeof(struct vfs_mount_info) : 0;
  size_t count = 0;
  while (vfs_mount_at(count)) {
    if (count < max) {
      struct vfs_mount_info out;
      if (!vfs_mount_detail(count, &out)) { return -EIO; }
      if (!syscall_user_writable(addr, sizeof(out)) ||
          !vmm_copy_to_user(syscall_active_as(), addr, &out, sizeof(out))) { return -EFAULT; }
      addr += sizeof(out);
    }
    ++count;
  }
  return count;
}

int64_t sys_pivot_root(uint64_t root_addr, uint64_t old_addr) {
  char root[CELL_PATH_MAX], old[CELL_PATH_MAX];
  int rc = syscall_copy_resolved_path_at(AT_FDCWD, root_addr, root, sizeof(root));
  if (rc) { return rc; }
  rc = syscall_copy_resolved_path_at(AT_FDCWD, old_addr, old, sizeof(old));
  return rc ? rc : mountns_pivot(root, old);
}

int64_t sys_mount(uint64_t source, uint64_t target, uint64_t type, uint64_t flags, uint64_t data) {
  if (!mountns_may_mount()) { return -EPERM; }
  char path[CELL_PATH_MAX];
  int rc = syscall_copy_resolved_path_at(AT_FDCWD, target, path, sizeof(path));
  if (rc) { return rc; }
  if (!cell_fs_path_allowed(path, CELL_FS_WRITE)) { return -EPERM; }
  constexpr uint64_t MS_PRIVATE = 1 << 18, MS_SLAVE = 1 << 19, MS_REC = 1 << 14, MS_SILENT = 1 << 15;
  constexpr uint64_t MS_NOSUID = 2, MS_NODEV = 4;
  if ((flags & 0xffff0000) == 0xc0ed0000) { flags &= 0xffff; }
  flags &= ~MS_SILENT;
  constexpr uint64_t MS_BIND = 4096;
  if ((flags & ~MS_REC) == MS_BIND) {
    char src[CELL_PATH_MAX];
    rc = syscall_copy_resolved_path_at(AT_FDCWD, source, src, sizeof(src));
    return rc ? rc : mountns_bind(src, path, flags & MS_REC);
  }
  if ((flags & ~MS_REC) == MS_PRIVATE || (flags & ~MS_REC) == MS_SLAVE) {
    const struct vfs_mount *m;
    for (size_t i = 0; (m = vfs_mount_at(i)); ++i) {
      if (str_eq(path, m->target)) { return 0; }
    }
    return -EINVAL;
  }
  if (flags & ~(MS_NOSUID | MS_NODEV)) { return -95; }
  char name[32];
  if ((rc = syscall_read_user_string(type, name, sizeof(name))) < 0) { return rc; }
  if (!str_eq(name, "tmpfs")) { return -19; }
  unsigned mode = 01777;
  if (data) {
    char options[32];
    if ((rc = syscall_read_user_string(data, options, sizeof(options))) < 0) { return rc; }
    if (*options) {
      if (!starts_with(options, "mode=") || !options[5]) { return -EINVAL; }
      mode = 0;
      for (const char *c = options + 5; *c; ++c) {
        if (*c < '0' || *c > '7' || mode > 0777) { return -EINVAL; }
        mode = mode * 8 + (*c - '0');
      }
    }
  }
  return mountns_tmpfs(path, mode, flags & MS_NOSUID, flags & MS_NODEV);
}

int64_t sys_umount2(uint64_t target, uint64_t flags) {
  if (!mountns_may_mount()) { return -EPERM; }
  if (flags & ~2ull) { return -EINVAL; }
  char path[CELL_PATH_MAX];
  int rc = syscall_copy_resolved_path_at(AT_FDCWD, target, path, sizeof(path));
  if (rc) { return rc; }
  if (!cell_fs_path_allowed(path, CELL_FS_WRITE)) { return -EPERM; }
  return mountns_unmount(path, flags & 2);
}
