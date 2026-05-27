#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mem.h"
#include "ramfs.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  AT_FDCWD = -100,
  F_OK = 0,
  X_OK = 1,
  W_OK = 2,
  R_OK = 4,
  ENOENT = 2,
  EPERM = 1,
  EBADF = 9,
  EFAULT = 14,
  ENOTDIR = 20,
  ENAMETOOLONG = 36,
};

static struct user_address_space *current_as;
static bool path_policy_denied;
static uint64_t realtime_base_sec;
static uint64_t realtime_base_cnt;
static uint64_t realtime_freq;

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

void syscall_set_address_space(struct user_address_space *as) {
  current_as = as;
}

void syscall_set_ramfs(struct ramfs *fs) {
  (void)fs;
}

void syscall_set_boot_time(uint64_t epoch_sec) {
  realtime_base_sec = epoch_sec;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(realtime_base_cnt));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(realtime_freq));
}

static struct user_address_space *active_as(void) {
  struct user_address_space *as = cell_current_as();
  return as == NULL ? current_as : as;
}

struct user_address_space *syscall_active_as(void) {
  return active_as();
}

void syscall_realtime_base(uint64_t *epoch_sec, uint64_t *counter, uint64_t *freq) {
  *epoch_sec = realtime_base_sec;
  *counter = realtime_base_cnt;
  *freq = realtime_freq;
}

static bool user_readable(uint64_t buf, uint64_t len) {
  return cell_ensure_user_range(buf, (size_t)len, VMM_ACCESS_READ) &&
         vmm_user_range_accessible(active_as(), buf, (size_t)len, VMM_ACCESS_READ);
}

static bool user_writable(uint64_t buf, uint64_t len) {
  return cell_ensure_user_range(buf, (size_t)len, VMM_ACCESS_WRITE) &&
         vmm_user_range_accessible(active_as(), buf, (size_t)len, VMM_ACCESS_WRITE);
}

bool syscall_user_readable(uint64_t buf, uint64_t len) {
  return user_readable(buf, len);
}

bool syscall_user_writable(uint64_t buf, uint64_t len) {
  return user_writable(buf, len);
}

static bool copy_string_from_user(uint64_t user, char *dst, size_t cap) {
  if (cap == 0) { return false; }
  for (size_t i = 0; i + 1 < cap; ++i) {
    if (!user_readable(user + i, 1) || !vmm_copy_from_user(active_as(), &dst[i], user + i, 1)) { return false; }
    if (dst[i] == '\0') { return true; }
  }
  dst[cap - 1] = '\0';
  return false;
}

bool syscall_copy_string_from_user(uint64_t user, char *dst, size_t cap) {
  return copy_string_from_user(user, dst, cap);
}

static bool normalize_path(const char *base, const char *path, char *out, size_t cap) {
  char input[256];
  size_t pos = 0;
  if (path[0] != '/') {
    for (size_t i = 0; base[i] != '\0'; ++i) {
      if (pos + 1 >= sizeof(input)) { return false; }
      input[pos++] = base[i];
    }
    if (pos == 0 || input[pos - 1] != '/') {
      if (pos + 1 >= sizeof(input)) { return false; }
      input[pos++] = '/';
    }
  }
  for (size_t i = 0; path[i] != '\0'; ++i) {
    if (pos + 1 >= sizeof(input)) { return false; }
    input[pos++] = path[i];
  }
  input[pos] = '\0';

  char components[16][32];
  size_t count = 0;
  const char *p = input;
  while (*p != '\0') {
    while (*p == '/') {
      ++p;
    }
    if (*p == '\0') { break; }
    char comp[32];
    size_t len = 0;
    while (p[len] != '\0' && p[len] != '/') {
      if (len + 1 >= sizeof(comp)) { return false; }
      comp[len] = p[len];
      ++len;
    }
    comp[len] = '\0';
    if (streq(comp, ".")) {
    } else if (streq(comp, "..")) {
      if (count > 0) { --count; }
    } else {
      if (count >= 16) { return false; }
      kmemcpy(components[count++], comp, len + 1);
    }
    p += len;
  }

  size_t out_pos = 0;
  out[out_pos++] = '/';
  for (size_t i = 0; i < count; ++i) {
    size_t len = kstrlen(components[i]);
    if (out_pos + len + (i + 1 < count ? 1 : 0) >= cap) { return false; }
    kmemcpy(out + out_pos, components[i], len);
    out_pos += len;
    if (i + 1 < count) { out[out_pos++] = '/'; }
  }
  out[out_pos] = '\0';
  return true;
}

bool syscall_normalize_path(const char *base, const char *path, char *out, size_t cap) {
  return normalize_path(base, path, out, cap);
}

static bool resolve_virtual_path(const char *virtual_path, char *out, size_t cap) {
  const char *chroot = cell_current_chroot();
  if (streq(chroot, "/")) {
    if (kstrlen(virtual_path) >= cap) { return false; }
    kmemcpy(out, virtual_path, kstrlen(virtual_path) + 1);
  } else if (streq(virtual_path, "/")) {
    if (kstrlen(chroot) >= cap) { return false; }
    kmemcpy(out, chroot, kstrlen(chroot) + 1);
  } else {
    size_t root_len = kstrlen(chroot);
    size_t path_len = kstrlen(virtual_path);
    if (root_len + path_len >= cap) { return false; }
    kmemcpy(out, chroot, root_len);
    kmemcpy(out + root_len, virtual_path, path_len + 1);
  }
  if (!cell_fs_path_allowed(out, 0)) {
    path_policy_denied = true;
    return false;
  }
  return true;
}

static bool copy_resolved_path(uint64_t path_addr, char *out, size_t cap) {
  path_policy_denied = false;
  char raw[128];
  char virtual_path[128];
  if (!copy_string_from_user(path_addr, raw, sizeof(raw)) ||
      !normalize_path(cell_current_cwd(), raw, virtual_path, sizeof(virtual_path))) {
    return false;
  }
  return resolve_virtual_path(virtual_path, out, cap);
}

bool syscall_copy_resolved_path(uint64_t path_addr, char *out, size_t cap) {
  return copy_resolved_path(path_addr, out, cap);
}

bool syscall_path_policy_denied(void) {
  return path_policy_denied;
}

static int64_t copy_resolved_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap) {
  path_policy_denied = false;
  char raw[128];
  if (!copy_string_from_user(path_addr, raw, sizeof(raw))) { return -(int64_t)EFAULT; }
  if (raw[0] == '/' || (int64_t)dirfd == AT_FDCWD) {
    return copy_resolved_path(path_addr, out, cap) ? 0 : (path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT);
  }

  if (!cell_fd_is_dir((int)dirfd)) { return cell_fd_path((int)dirfd, out, cap) ? -(int64_t)ENOTDIR : -(int64_t)EBADF; }
  char base[128];
  char virtual_path[128];
  if (!cell_fd_path((int)dirfd, base, sizeof(base))) { return -(int64_t)EBADF; }
  if (!normalize_path(base, raw, virtual_path, sizeof(virtual_path))) { return -(int64_t)ENAMETOOLONG; }
  return resolve_virtual_path(virtual_path, out, cap) ? 0 : (path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT);
}

int64_t syscall_copy_resolved_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap) {
  return copy_resolved_path_at(dirfd, path_addr, out, cap);
}

static bool copy_virtual_path(uint64_t path_addr, char *out, size_t cap) {
  char raw[128];
  return copy_string_from_user(path_addr, raw, sizeof(raw)) && normalize_path(cell_current_cwd(), raw, out, cap);
}

bool syscall_copy_virtual_path(uint64_t path_addr, char *out, size_t cap) {
  return copy_virtual_path(path_addr, out, cap);
}

static int64_t copy_virtual_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap) {
  char raw[128];
  if (!copy_string_from_user(path_addr, raw, sizeof(raw))) { return -(int64_t)EFAULT; }
  if (raw[0] == '/' || (int64_t)dirfd == AT_FDCWD) {
    return normalize_path(cell_current_cwd(), raw, out, cap) ? 0 : -(int64_t)ENAMETOOLONG;
  }
  if (!cell_fd_is_dir((int)dirfd)) { return cell_fd_path((int)dirfd, out, cap) ? -(int64_t)ENOTDIR : -(int64_t)EBADF; }
  char base[128];
  if (!cell_fd_path((int)dirfd, base, sizeof(base))) { return -(int64_t)EBADF; }
  return normalize_path(base, raw, out, cap) ? 0 : -(int64_t)ENAMETOOLONG;
}

int64_t syscall_copy_virtual_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap) {
  return copy_virtual_path_at(dirfd, path_addr, out, cap);
}

bool syscall_node_access_allowed(const struct vfs_node *node, uint64_t mask) {
  if (mask == F_OK || cell_current_euid() == 0) { return true; }
  uint32_t bits = node->mode & 0777u;
  if (cell_current_euid() == node->uid) {
    bits >>= 6;
  } else if (cell_current_egid() == node->gid) {
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

