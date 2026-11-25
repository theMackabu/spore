#pragma once

#include "handlers.h"
#include "path.h"

enum {
  AT_FDCWD = -100,
  AT_EMPTY_PATH = 0x1000,
  AT_SYMLINK_NOFOLLOW = 0x100,
  AT_SYMLINK_FOLLOW = 0x400,
  AT_EACCESS = 0x200,
  X_OK = 1,
  W_OK = 2,
  R_OK = 4,
  O_TMPFILE_BIT = 020000000,
  S_IFMT = 0170000,
  S_IFIFO = 0010000,
  S_IFLNK = 0120000,
  S_IFREG = 0100000,
  ELOOP = 40,
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
  EOPNOTSUPP = 95,
  EBUSY = 16,
  E2BIG = 7,
  STATX_BASIC_STATS = 0x000007ff,
};

int syscall_lookup_at(uint64_t dirfd, uint64_t addr, uint64_t flags, uint8_t rights, struct vfs_node *node,
                      char *resolved);
bool syscall_proc_fd_number(const char *path, int *fd);
