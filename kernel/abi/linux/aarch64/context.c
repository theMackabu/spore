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
  F_OK = 0,
  X_OK = 1,
  W_OK = 2,
  R_OK = 4,
  EFAULT = 14,
  E2BIG = 7,
};

static struct user_address_space *current_as;
static uint64_t realtime_base_sec;
static uint64_t realtime_base_cnt;
static uint64_t realtime_freq;

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
  int64_t length = syscall_read_user_string(user, dst, cap);
  if (length == -E2BIG) { dst[cap - 1] = '\0'; }
  return length > 0;
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
