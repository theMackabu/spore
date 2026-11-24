#include "../usercopy.h"
#include "handlers.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/info.h"
#include "proc/accounting.h"
#include "spore_version.h"

enum { EPERM = 1, EFAULT = 14, EINVAL = 22 };

struct utsname64 {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

struct sysinfo64 {
  int64_t uptime;
  uint64_t loads[3];
  uint64_t totalram;
  uint64_t freeram;
  uint64_t sharedram;
  uint64_t bufferram;
  uint64_t totalswap;
  uint64_t freeswap;
  uint16_t procs;
  uint16_t pad;
  uint64_t totalhigh;
  uint64_t freehigh;
  uint32_t mem_unit;
};

static void uts_copy(char dst[65], const char *src) {
  size_t i = 0;
  while (i < 64 && src[i] != '\0') {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

static char kernel_hostname[65] = SPORE_UTS_NODENAME;

int64_t sys_uname(uint64_t buf) {
  struct utsname64 u;
  kmemset(&u, 0, sizeof(u));
  uts_copy(u.sysname, SPORE_UTS_SYSNAME);
  uts_copy(u.nodename, kernel_hostname);
  uts_copy(u.release, SPORE_UTS_RELEASE);
  uts_copy(u.version, SPORE_UTS_VERSION);
  uts_copy(u.machine, SPORE_UTS_MACHINE);
  uts_copy(u.domainname, SPORE_UTS_DOMAINNAME);
  return syscall_user_writable(buf, sizeof(u)) && vmm_copy_to_user(syscall_active_as(), buf, &u, sizeof(u))
           ? 0
           : -(int64_t)EFAULT;
}

int64_t sys_sethostname(uint64_t name_addr, uint64_t len) {
  if (cell_current_euid() != 0) { return -(int64_t)EPERM; }
  if (len == 0 || len >= sizeof(kernel_hostname) || !syscall_user_readable(name_addr, len)) { return -(int64_t)EINVAL; }
  if (!vmm_copy_from_user(syscall_active_as(), kernel_hostname, name_addr, (size_t)len)) { return -(int64_t)EFAULT; }
  kernel_hostname[len] = '\0';
  return 0;
}

int64_t sys_sysinfo(uint64_t info_addr) {
  if (info_addr == 0 || !syscall_user_writable(info_addr, sizeof(struct sysinfo64))) { return -(int64_t)EFAULT; }
  struct sysinfo64 info;
  kmemset(&info, 0, sizeof(info));
  uint64_t total_pages = pmm_total_pages();
  uint64_t free_pages = pmm_free_pages();
  info.uptime = (int64_t)(cell_uptime_ticks() / 100);
  info.totalram = total_pages;
  info.freeram = free_pages;
  info.procs = (uint16_t)cell_proc_info(NULL, 0);
  info.mem_unit = PAGE_SIZE;
  return vmm_copy_to_user(syscall_active_as(), info_addr, &info, sizeof(info)) ? 0 : -(int64_t)EFAULT;
}
