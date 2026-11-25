#include "../usercopy.h"
#include "file_abi.h"
#include "fs/mount_namespace.h"
#include "fs/vfs.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pmm.h"
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
          !vmm_copy_to_user(syscall_active_as(), addr, &out, sizeof(out))) {
        return -EFAULT;
      }
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

static bool tmpfs_pages(const char *s, uint64_t *pages) {
  if (*s < '0' || *s > '9') { return false; }
  uint64_t n = 0;
  while (*s >= '0' && *s <= '9') {
    unsigned digit = *s++ - '0';
    if (n > (UINT64_MAX - digit) / 10) { return false; }
    n = n * 10 + digit;
  }
  if (str_eq(s, "%")) {
    if (!n || n > 100) { return false; }
    *pages = (pmm_total_pages() * n + 99) / 100;
    return true;
  }
  unsigned shift = 0;
  if (*s) {
    if (s[1]) { return false; }
    switch (*s) {
    case 'k':
    case 'K':
      shift = 10;
      break;
    case 'm':
    case 'M':
      shift = 20;
      break;
    case 'g':
    case 'G':
      shift = 30;
      break;
    default:
      return false;
    }
  }
  if (n > (UINT64_MAX >> shift)) { return false; }
  uint64_t bytes = n << shift;
  *pages = bytes / RAMFS_PAGE_SIZE + (bytes % RAMFS_PAGE_SIZE != 0);
  return true;
}

int64_t sys_mount(uint64_t source, uint64_t target, uint64_t type, uint64_t flags, uint64_t data) {
  if (!mountns_may_mount()) { return -EPERM; }
  char path[CELL_PATH_MAX];
  int rc = syscall_copy_resolved_path_at(AT_FDCWD, target, path, sizeof(path));
  if (rc) { return rc; }
  if (!cell_fs_path_allowed(path, CELL_FS_WRITE)) { return -EPERM; }
  constexpr uint64_t MS_PRIVATE = 1 << 18, MS_SLAVE = 1 << 19, MS_REC = 1 << 14, MS_SILENT = 1 << 15;
  constexpr uint64_t mount_flags = MOUNT_RDONLY | MOUNT_NOSUID | MOUNT_NODEV | MOUNT_NOEXEC;
  if ((flags & 0xffff0000) == 0xc0ed0000) { flags &= 0xffff; }
  flags &= ~MS_SILENT;
  constexpr uint64_t MS_BIND = 4096;
  constexpr uint64_t MS_REMOUNT = 32;
  if ((flags & ~mount_flags) == (MS_BIND | MS_REMOUNT)) { return mountns_remount(path, flags & mount_flags); }
  if ((flags & ~MS_REC) == MS_BIND) {
    char src[CELL_PATH_MAX];
    rc = syscall_copy_resolved_path_at(AT_FDCWD, source, src, sizeof(src));
    return rc ? rc : mountns_bind(src, path, flags & MS_REC);
  }
  constexpr uint64_t MS_SHARED = 1 << 20;
  uint64_t propagation = flags & ~MS_REC;
  if (propagation == MS_PRIVATE || propagation == MS_SLAVE || propagation == MS_SHARED) {
    return mountns_propagation(path, propagation == MS_SHARED, propagation == MS_SLAVE, flags & MS_REC);
  }
  if (flags & ~mount_flags) { return -95; }
  char name[32];
  if ((rc = syscall_read_user_string(type, name, sizeof(name))) < 0) { return rc; }
  if (str_eq(name, "proc")) { return data ? -EINVAL : mountns_proc(path, flags); }
  bool pts = str_eq(name, "devpts");
  if (!pts && !str_eq(name, "tmpfs")) { return -19; }
  struct devpts_options settings = {.mode = 0600};
  unsigned mode = pts ? 0755 : 01777;
  bool fresh = false;
  uint64_t page_limit = 0;
  if (data) {
    char options[128];
    if ((rc = syscall_read_user_string(data, options, sizeof(options))) < 0) { return rc; }
    char *p = options;
    while (*p) {
      char *start = p;
      while (*p && *p != ',') {
        ++p;
      }
      if (*p) { *p++ = 0; }
      if (pts && str_eq(start, "newinstance")) {
        fresh = true;
        continue;
      }
      if (!pts && starts_with(start, "size=")) {
        if (!tmpfs_pages(start + 5, &page_limit)) { return -EINVAL; }
        continue;
      }
      unsigned *value;
      if (starts_with(start, "mode=")) {
        value = pts ? &settings.mode : &mode;
        start += 5;
      } else if (pts && starts_with(start, "ptmxmode=")) {
        value = &settings.ptmxmode;
        start += 9;
      } else {
        return -EINVAL;
      }
      if (!*start) { return -EINVAL; }
      unsigned n = 0;
      for (; *start; ++start) {
        if (*start < '0' || *start > '7' || n > 0777) { return -EINVAL; }
        n = n * 8 + (*start - '0');
      }
      *value = n;
    }
  }
  if (pts && !fresh) { return -95; }
  return mountns_memory(path, mode, flags, page_limit, pts ? &settings : NULL);
}

int64_t sys_umount2(uint64_t target, uint64_t flags) {
  if (!mountns_may_mount()) { return -EPERM; }
  if (flags & ~2ull) { return -EINVAL; }
  char dot[2];
  if (syscall_read_user_string(target, dot, sizeof(dot)) == 2 && dot[0] == '.' && !cell_current_cwd()[1] &&
      !cell_current_chroot()[1]) {
    if (!cell_fs_path_allowed("/", CELL_FS_WRITE)) { return -EPERM; }
    return mountns_unstack(flags & 2);
  }
  char path[CELL_PATH_MAX];
  int rc = syscall_copy_resolved_path_at(AT_FDCWD, target, path, sizeof(path));
  if (rc) { return rc; }
  if (!cell_fs_path_allowed(path, CELL_FS_WRITE)) { return -EPERM; }
  return mountns_unmount(path, flags & 2);
}
