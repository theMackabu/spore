#include "../usercopy.h"
#include "file_abi.h"
#include "fs/vfs.h"
#include "fs/mount_namespace.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/capabilities.h"
#include "proc/fd.h"
#include "proc/policy.h"

size_t spore_normalize_path(const char *base, size_t base_len, const char *raw, size_t len, char *out, size_t cap);

static int resolve_from(const char *base, size_t base_len, const char *raw, size_t len, char *visible, char *out,
                        size_t cap) {
  if (mountns_current_routes() || ramfs_has_symlinks() || vfs_path_has_proc(raw, len) || vfs_path_has_proc(base, base_len)) {
    const char *root = cell_current_chroot();
    int rc = vfs_walk(root, base, base_len, raw, len, false, out, cap);
    if (rc) { return rc; }
    const char *relative = root[1] ? out + kstrlen(root) : out;
    copy_cstr(visible, CELL_PATH_MAX, *relative ? relative : "/");
    return cell_fs_path_allowed(out, 0) ? 0 : -EPERM;
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

int syscall_dirfd_path(int fd, char path[CELL_PATH_MAX]) {
  if (!cell_fd_path(fd, path, CELL_PATH_MAX)) { return -EBADF; }
  if (!cell_fd_is_dir(fd)) { return -ENOTDIR; }
  struct open_file *file = cell_file_at(cell_current_domain_internal(), fd);
  if (!file || file->type != OPEN_VFS) { return 0; }
  bool mounted_fd = file->node.backend == VFS_RAMFS && file->node.ramfs.fs->device;
  if (!mounted_fd && !mountns_has_mounts()) { return 0; }
  char visible[CELL_PATH_MAX], resolved[CELL_PATH_MAX];
  int rc = resolve_from("/", 1, path, kstrlen(path), visible, resolved, sizeof(resolved));
  if (rc) { return rc; }
  if (mounted_fd || mountns_lookup(resolved).mount_id) {
    struct vfs_node named;
    if (!vfs_lookup(resolved, &named) || (named.mount_id ? named.mount_id : named.dev_id) != (file->node.mount_id ? file->node.mount_id : file->node.dev_id) || named.dev_id != file->node.dev_id || named.ino != file->node.ino) {
      if (cell_current_chroot()[1]) { return -116; }
      rc = mountns_relocate_dir(&file->node, file->path, path, CELL_PATH_MAX);
      if (rc || !vfs_lookup(path, &named) || named.ino != file->node.ino || named.dev_id != file->node.dev_id) { return -116; }
      return 0;
    }
  }
  return 0;
}

static int resolve_at(uint64_t dirfd, const char *raw, size_t len, char *visible, char *out, size_t cap) {
  if (!len) { return -ENOENT; }
  char dir[CELL_PATH_MAX];
  const char *base = "/";
  size_t base_len = 1;
  if (raw[0] != '/') {
    if ((int)dirfd == AT_FDCWD) {
      if (cell_current_domain_internal()->cwd_covered_mount) { return -116; }
      base = cell_current_cwd();
    } else {
      int rc = syscall_dirfd_path((int)dirfd, dir);
      if (rc) { return rc; }
      base = dir;
    }
    base_len = kstrlen(base);
  }
  return resolve_from(base, base_len, raw, len, visible, out, cap);
}

int64_t syscall_resolve_path_at(uint64_t dirfd, const char *raw, char *out, size_t cap) {
  char visible[CELL_PATH_MAX];
  return resolve_at(dirfd, raw, kstrlen(raw), visible, out, cap);
}

int syscall_follow_final_links(char path[CELL_PATH_MAX]) {
  const char *root = cell_current_chroot();
  size_t prefix = root[1] ? kstrlen(root) : 0;
  for (unsigned depth = 0;; ++depth) {
    struct vfs_node node;
    if (!vfs_lstat(path, &node)) { return -ENOENT; }
    if ((node.mode & 0170000) != S_IFLNK) { return 0; }
    if (depth == 40) { return -ELOOP; }
    char target[CELL_PATH_MAX], visible[CELL_PATH_MAX];
    size_t len;
    if (node.size >= sizeof(target)) { return -ENAMETOOLONG; }
    if (!vfs_readlink(&node, target, sizeof(target), &len) || !len) { return -ENOENT; }
    const char *base = path + prefix;
    size_t parent = kstrlen(base);
    while (parent && base[parent - 1] != '/') {
      --parent;
    }
    int rc = resolve_from(base, parent, target, len, visible, path, CELL_PATH_MAX);
    if (rc) { return rc; }
  }
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

bool syscall_proc_fd_number(const char *path, int *fd_out) {
  const char *prefix = "/proc/self/fd/";
  if (!starts_with(path, prefix)) { return false; }
  if (mountns_lookup(path).fs) { return false; }
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

int syscall_lookup_at(uint64_t dirfd, uint64_t addr, uint64_t flags, uint8_t rights, struct vfs_node *node,
                      char *resolved) {
  char raw[CELL_PATH_MAX], local[CELL_PATH_MAX];
  char *path = resolved ? resolved : local;
  path[0] = 0;
  int64_t size = addr || !(flags & AT_EMPTY_PATH) ? syscall_read_user_string(addr, raw, sizeof(raw)) : 1;
  if (size < 0) { return size == -E2BIG ? -ENAMETOOLONG : (int)size; }
  if (size == 1) {
    if (!(flags & AT_EMPTY_PATH)) { return -ENOENT; }
    if ((int)dirfd != AT_FDCWD) { return cell_fd_stat((int)dirfd, node) ? 0 : -EBADF; }
    raw[0] = '.';
    raw[1] = 0;
  }
  int rc = syscall_resolve_path_at(dirfd, raw, path, CELL_PATH_MAX);
  if (rc) { return rc; }
  if (rights && !cell_fs_path_allowed(path, rights)) { return -EPERM; }
  bool directory = size > 1 && raw[size - 2] == '/';
  bool found = flags & AT_SYMLINK_NOFOLLOW && !directory ? vfs_lstat(path, node) : vfs_lookup(path, node);
  if (!found) { return -ENOENT; }
  return directory && !node->is_dir ? -ENOTDIR : 0;
}


int syscall_may_change_entry(char *path, bool replacing) {
  struct ramfs_route route = mountns_lookup(path);
  if ((route.fs && route.fs->devpts) || (route.access && route.access->proc_mount)) { return -EPERM; }
  if (vfs_readonly_path(path)) { return -30; }
  if (!cell_fs_path_allowed(path, CELL_FS_WRITE)) { return -EPERM; }
  size_t component = base_name(path) - path;
  size_t split = component > 1 ? component - 1 : 1;
  char saved = path[split];
  path[split] = 0;
  struct vfs_node parent;
  bool found = vfs_lookup(path, &parent);
  path[split] = saved;
  if (!found) { return -ENOENT; }
  if (!parent.is_dir) { return -ENOTDIR; }
  if (!syscall_node_access_allowed(&parent, X_OK)) { return -EACCES; }
  if (!syscall_node_access_allowed(&parent, W_OK)) {
    struct vfs_node existing;
    return !replacing && vfs_lstat(path, &existing) ? -EEXIST : -EACCES;
  }
  uint32_t euid = cell_current_euid();
  if (replacing && (parent.mode & 01000u) && !cell_capable(CAP_FOWNER) && euid != parent.uid) {
    struct vfs_node victim;
    if (vfs_lstat(path, &victim) && euid != victim.uid) { return -EPERM; }
  }
  return 0;
}

bool syscall_node_access_allowed(const struct vfs_node *node, uint64_t mask) {
  if (mask == 0) { return true; }
  if (cell_capable(CAP_DAC_OVERRIDE) && (!(mask & X_OK) || node->is_dir || (node->mode & 0111))) { return true; }
  if (cell_capable(CAP_DAC_READ_SEARCH) && !(mask & W_OK) && (!(mask & X_OK) || node->is_dir)) { return true; }
  uint32_t bits = node->mode & 0777u;
  if (cell_current_euid() == node->uid) {
    bits >>= 6;
  } else if (cell_in_group(node->gid)) {
    bits >>= 3;
  }
  if ((mask & R_OK) != 0 && (bits & 04u) == 0) { return false; }
  if ((mask & W_OK) != 0 && (bits & 02u) == 0) { return false; }
  if ((mask & X_OK) != 0 && (bits & 01u) == 0) { return false; }
  return true;
}

uint8_t syscall_fs_rights_from_access(uint64_t access) {
  uint8_t rights = 0;
  if ((access & R_OK) != 0) { rights |= CELL_FS_READ; }
  if ((access & W_OK) != 0) { rights |= CELL_FS_WRITE; }
  if ((access & X_OK) != 0) { rights |= CELL_FS_EXEC; }
  return rights;
}
