#include "usercopy.h"
#include "abi/linux/aarch64/syscall.h"
#include "mm/pmm.h"
#include "proc/memory.h"
#include "proc/domain.h"

enum { EFAULT = 14, E2BIG = 7 };

static struct user_address_space *current_as;

void syscall_set_address_space(struct user_address_space *as) {
  current_as = as;
}

struct user_address_space *syscall_active_as(void) {
  struct user_address_space *as = cell_current_as();
  return as == NULL ? current_as : as;
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
