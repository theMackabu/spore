#include "internal.h"
#include "lib/mem.h"
#include "proc/fd.h"
#include "proc/limits.h"

bool vfs_path_has_proc(const char *s, size_t len) {
  for (size_t i = 0; i + 4 <= len; ++i) {
    if (s[i] == 'p' && (!i || s[i - 1] == '/') && !kmemcmp(s + i, "proc", 4) &&
        (i + 4 == len || s[i + 4] == '/')) { return true; }
  }
  return false;
}

static void up(char *out, size_t *length, size_t floor) {
  while (*length > floor && out[*length - 1] != '/') { --*length; }
  if (*length > floor) { --*length; }
  out[*length] = 0;
}

int vfs_walk(const char *root, const char *base, size_t base_len, const char *path, size_t length,
             bool follow_final, char *out, size_t cap) {
  char todo[2][2 * CELL_PATH_MAX + 2];
  size_t prefix = length && *path == '/' ? 0 : base_len;
  if (prefix + length + 2 > sizeof(todo[0])) { return -36; }
  kmemcpy(todo[0], base, prefix);
  if (prefix) { todo[0][prefix++] = '/'; }
  kmemcpy(todo[0] + prefix, path, length);
  todo[0][prefix + length] = 0;
  size_t floor = kstrlen(root), used = floor;
  if (!floor || floor >= cap) { return -36; }
  kmemcpy(out, root, floor + 1);
  unsigned links = 0;
  const char *p = todo[0];
  for (;;) {
    while (*p == '/') { ++p; }
    if (!*p) { return 0; }
    const char *name = p;
    while (*p && *p != '/') { ++p; }
    size_t n = p - name;
    if (n == 1 && *name == '.') { continue; }
    if (n == 2 && name[0] == '.' && name[1] == '.') { up(out, &used, floor); continue; }
    size_t parent = used;
    if (used + (used > 1) + n >= cap) { return -36; }
    if (used > 1) { out[used++] = '/'; }
    kmemcpy(out + used, name, n);
    used += n;
    out[used] = 0;
    if (!*p && !follow_final) { return 0; }
    struct vfs_node node;
    if (!vfs_lookup_component(out, true, &node)) { return -2; }
    if ((node.backend == VFS_EXT2 || node.backend == VFS_RAMFS) && (node.mode & 0170000) == 0120000) {
      if (++links > 40) { return -40; }
      char *next = todo[links % 2];
      size_t len;
      if (!node.size || node.size >= CELL_PATH_MAX ||
          !vfs_readlink(&node, next, CELL_PATH_MAX, &len)) { return -36; }
      size_t suffix = kstrlen(p);
      if (len + suffix >= sizeof(todo[0])) { return -36; }
      kmemcpy(next + len, p, suffix + 1);
      used = *next == '/' ? floor : parent;
      out[used] = 0;
      p = next;
    } else if (*p && node.backend == VFS_PROC && node.device == RAMFS_DEV_PROC_PID_FD && node.proc.fd >= 0) {
      struct vfs_node target;
      if (!cell_fd_stat(node.proc.fd, &target)) { return -2; }
      if (!target.is_dir) { return -20; }
      int rc = vfs_directory_path(&target, root, out, cap);
      if (rc) { return rc; }
      used = kstrlen(out);
    } else if (*p && !node.is_dir) {
      if (node.backend != VFS_PROC || !vfs_lookup_component(out, false, &node) || !node.is_dir) { return -20; }
    }
  }
}
