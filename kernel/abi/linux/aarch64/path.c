#include "handlers.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/policy.h"

enum { AT_FDCWD = -100, EPERM = 1, ENOENT = 2, E2BIG = 7, EBADF = 9, ENOTDIR = 20, ENAMETOOLONG = 36 };

size_t spore_normalize_path(const char *base, size_t base_len, const char *raw, size_t len, char *out, size_t cap);

static int resolve_at(uint64_t dirfd, const char *raw, size_t len, char *visible, char *out, size_t cap) {
  if (!len) { return -ENOENT; }
  char dir[CELL_PATH_MAX];
  const char *base = "/";
  size_t base_len = 1;
  if (raw[0] != '/') {
    if ((int)dirfd == AT_FDCWD) {
      base = cell_current_cwd();
    } else {
      if (!cell_fd_path((int)dirfd, dir, sizeof(dir))) { return -EBADF; }
      if (!cell_fd_is_dir((int)dirfd)) { return -ENOTDIR; }
      base = dir;
    }
    base_len = kstrlen(base);
  }
  size_t name_len = spore_normalize_path(base, base_len, raw, len, visible, CELL_PATH_MAX);
  if (!name_len) { return -ENAMETOOLONG; }
  const char *root = cell_current_chroot();
  size_t prefix = root[1] ? kstrlen(root) : 0;
  if (prefix && name_len == 1) { name_len = 0; }
  if (prefix + name_len >= cap) { return -ENAMETOOLONG; }
  if (prefix) { kmemcpy(out, root, prefix); }
  kmemcpy(out + prefix, visible, name_len);
  out[prefix + name_len] = 0;
  return cell_fs_path_allowed(out, 0) ? 0 : -EPERM;
}

int64_t syscall_resolve_path_at(uint64_t dirfd, const char *raw, char *out, size_t cap) {
  char visible[CELL_PATH_MAX];
  return resolve_at(dirfd, raw, kstrlen(raw), visible, out, cap);
}

static int copy_path_at(uint64_t dirfd, uint64_t addr, char *visible, char *out, size_t cap, bool *trailing_slash) {
  char raw[CELL_PATH_MAX];
  int64_t size = syscall_read_user_string(addr, raw, sizeof(raw));
  if (size < 0) { return size == -E2BIG ? -ENAMETOOLONG : (int)size; }
  if (trailing_slash) { *trailing_slash = size > 1 && raw[size - 2] == '/'; }
  return resolve_at(dirfd, raw, (size_t)size - 1, visible, out, cap);
}

int64_t syscall_read_path_at(uint64_t dirfd, uint64_t addr, struct syscall_path *path) {
  return copy_path_at(dirfd, addr, path->visible, path->resolved, sizeof(path->resolved), &path->trailing_slash);
}

int64_t syscall_copy_resolved_path_at(uint64_t dirfd, uint64_t addr, char *out, size_t cap) {
  char visible[CELL_PATH_MAX];
  return copy_path_at(dirfd, addr, visible, out, cap, NULL);
}
