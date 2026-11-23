#include "arch/aarch64/timer.h"
#include "abi/linux/aarch64/syscall.h"
#include "handlers.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/policy.h"
#include "fs/vfs.h"

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
  E2BIG = 7,
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

void syscall_set_boot_time(uint64_t epoch_sec) {
  realtime_base_sec = epoch_sec;
  realtime_base_cnt = aarch64_counter_value();
  realtime_freq = aarch64_counter_frequency();
}

struct user_address_space *syscall_active_as(void) {
  struct user_address_space *as = cell_current_as();
  return as == NULL ? current_as : as;
}

void syscall_realtime_base(uint64_t *epoch_sec, uint64_t *counter, uint64_t *freq) {
  *epoch_sec = realtime_base_sec;
  *counter = realtime_base_cnt;
  *freq = realtime_freq;
}

static bool user_accessible(uint64_t addr, size_t len, enum vmm_access access) {
  if (len == 0) { return true; }
  struct domain *domain = cell_current_domain_internal();
  if (domain != NULL) { return cell_domain_ensure_user_range(domain, addr, len, access); }
  return vmm_user_range_accessible(current_as, addr, len, access);
}

bool syscall_user_readable(uint64_t buf, uint64_t len) {
  return user_accessible(buf, len, VMM_ACCESS_READ);
}

bool syscall_user_writable(uint64_t buf, uint64_t len) {
  return user_accessible(buf, len, VMM_ACCESS_WRITE);
}

int64_t syscall_read_user_string(uint64_t user, char *dst, size_t limit) {
  struct user_address_space *as = syscall_active_as();
  size_t done = 0;
  while (done < limit) {
    uint64_t addr = user + done;
    if (addr < user) { return -EFAULT; }
    uint64_t pa = vmm_user_to_phys(as, addr);
    if (pa == 0) {
      if (!cell_ensure_user_range(addr, 1, VMM_ACCESS_READ)) { return -EFAULT; }
      pa = vmm_user_to_phys(as, addr);
      if (pa == 0) { return -EFAULT; }
    }
    size_t chunk = PAGE_SIZE - addr % PAGE_SIZE;
    if (chunk > limit - done) { chunk = limit - done; }
    const char *src = (const char *)(uintptr_t)(as->hhdm_offset + pa);
    for (size_t i = 0; i < chunk; ++i) {
      char c = src[i];
      if (dst != NULL) { dst[done + i] = c; }
      if (c == '\0') { return (int64_t)(done + i + 1); }
    }
    done += chunk;
  }
  return -E2BIG;
}

bool syscall_copy_string_from_user(uint64_t user, char *dst, size_t cap) {
  if (cap == 0) { return false; }
  int64_t length = syscall_read_user_string(user, dst, cap - 1);
  if (length == -E2BIG) { dst[cap - 1] = '\0'; }
  return length > 0;
}

bool syscall_normalize_path(const char *base, const char *path, char *out, size_t cap) {
  if (cap < 2) { return false; }
  char input[CELL_PATH_MAX];
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

  struct {
    const char *start;
    size_t len;
  } components[16];
  size_t count = 0;
  const char *p = input;
  while (*p != '\0') {
    while (*p == '/') {
      ++p;
    }
    if (*p == '\0') { break; }
    const char *start = p;
    while (*p != '\0' && *p != '/') { ++p; }
    size_t len = (size_t)(p - start);
    if (len == 1 && start[0] == '.') { continue; }
    if (len == 2 && start[0] == '.' && start[1] == '.') {
      if (count > 0) { --count; }
      continue;
    }
    if (count == sizeof(components) / sizeof(components[0])) { return false; }
    components[count].start = start;
    components[count++].len = len;
  }

  size_t out_pos = 0;
  out[out_pos++] = '/';
  for (size_t i = 0; i < count; ++i) {
    size_t len = components[i].len;
    if (out_pos + len + (i + 1 < count ? 1 : 0) >= cap) { return false; }
    kmemcpy(out + out_pos, components[i].start, len);
    out_pos += len;
    if (i + 1 < count) { out[out_pos++] = '/'; }
  }
  out[out_pos] = '\0';
  return true;
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

bool syscall_copy_resolved_path(uint64_t path_addr, char *out, size_t cap) {
  path_policy_denied = false;
  char raw[CELL_PATH_MAX];
  char virtual_path[CELL_PATH_MAX];
  if (!syscall_copy_string_from_user(path_addr, raw, sizeof(raw)) ||
      !syscall_normalize_path(cell_current_cwd(), raw, virtual_path, sizeof(virtual_path))) {
    return false;
  }
  return resolve_virtual_path(virtual_path, out, cap);
}

bool syscall_path_policy_denied(void) {
  return path_policy_denied;
}

int64_t syscall_copy_resolved_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap) {
  path_policy_denied = false;
  char raw[CELL_PATH_MAX];
  if (!syscall_copy_string_from_user(path_addr, raw, sizeof(raw))) { return -(int64_t)EFAULT; }
  char dir_path[CELL_PATH_MAX];
  char virtual_path[CELL_PATH_MAX];
  const char *base = cell_current_cwd();
  bool relative_to_fd = raw[0] != '/' && (int64_t)dirfd != AT_FDCWD;
  if (relative_to_fd) {
    if (!cell_fd_path((int)dirfd, dir_path, sizeof(dir_path))) { return -(int64_t)EBADF; }
    if (!cell_fd_is_dir((int)dirfd)) { return -(int64_t)ENOTDIR; }
    base = dir_path;
  }
  if (!syscall_normalize_path(base, raw, virtual_path, sizeof(virtual_path))) {
    return relative_to_fd ? -(int64_t)ENAMETOOLONG : -(int64_t)EFAULT;
  }
  return resolve_virtual_path(virtual_path, out, cap) ? 0 : (path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT);
}

bool syscall_copy_virtual_path(uint64_t path_addr, char *out, size_t cap) {
  char raw[CELL_PATH_MAX];
  return syscall_copy_string_from_user(path_addr, raw, sizeof(raw)) && syscall_normalize_path(cell_current_cwd(), raw, out, cap);
}

int64_t syscall_copy_virtual_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap) {
  char raw[CELL_PATH_MAX];
  if (!syscall_copy_string_from_user(path_addr, raw, sizeof(raw))) { return -(int64_t)EFAULT; }
  if (raw[0] == '/' || (int64_t)dirfd == AT_FDCWD) {
    return syscall_normalize_path(cell_current_cwd(), raw, out, cap) ? 0 : -(int64_t)ENAMETOOLONG;
  }
  if (!cell_fd_is_dir((int)dirfd)) { return cell_fd_path((int)dirfd, out, cap) ? -(int64_t)ENOTDIR : -(int64_t)EBADF; }
  char base[CELL_PATH_MAX];
  if (!cell_fd_path((int)dirfd, base, sizeof(base))) { return -(int64_t)EBADF; }
  return syscall_normalize_path(base, raw, out, cap) ? 0 : -(int64_t)ENAMETOOLONG;
}

bool syscall_node_access_allowed(const struct vfs_node *node, uint64_t mask) {
  if (mask == F_OK || cell_current_euid() == 0) { return true; }
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
