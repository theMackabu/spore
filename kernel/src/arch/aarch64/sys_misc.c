#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "exec/stack.h"
#include "arch/aarch64/smp.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "net.h"
#include "spore_version.h"

#include <stddef.h>
#include <stdint.h>

enum {
  PR_GET_DUMPABLE = 3,
  PR_SET_DUMPABLE = 4,
  PR_SET_NAME = 15,
  PR_GET_NAME = 16,
  PR_SET_NO_NEW_PRIVS = 38,
  PR_GET_NO_NEW_PRIVS = 39,
  PR_SET_VMA = 0x53564d41,
  PR_SET_VMA_ANON_NAME = 0,
  EPERM = 1,
  EFAULT = 14,
  EINVAL = 22,
};

struct net_config64 {
  uint32_t local_ip;
  uint32_t gateway_ip;
  uint32_t netmask;
  uint32_t dns_ip;
  uint32_t configured;
};

struct rlimit64 {
  uint64_t rlim_cur;
  uint64_t rlim_max;
};

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

int64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_limit, uint64_t old_limit) {
  enum {
    RLIMIT_STACK = 3,
    RLIMIT_NOFILE = 7,
    RLIMIT_AS = 9,
  };
  if (pid != 0 && (int)pid != cell_current_pid()) { return -(int64_t)EINVAL; }
  if (new_limit != 0) {
    struct rlimit64 ignored;
    if (!syscall_user_readable(new_limit, sizeof(ignored)) ||
        !vmm_copy_from_user(syscall_active_as(), &ignored, new_limit, sizeof(ignored))) {
      return -(int64_t)EFAULT;
    }
  }
  if (old_limit == 0) { return 0; }

  uint64_t unlimited = UINT64_MAX;
  struct rlimit64 out = {.rlim_cur = unlimited, .rlim_max = unlimited};
  if (resource == RLIMIT_STACK) {
    out.rlim_cur = USER_STACK_SIZE;
    out.rlim_max = USER_STACK_SIZE;
  } else if (resource == RLIMIT_NOFILE) {
    out.rlim_cur = MAX_FDS;
    out.rlim_max = MAX_FDS;
  } else if (resource == RLIMIT_AS) {
    out.rlim_cur = USER_STACK_TOP;
    out.rlim_max = USER_STACK_TOP;
  }
  return syscall_user_writable(old_limit, sizeof(out)) &&
             vmm_copy_to_user(syscall_active_as(), old_limit, &out, sizeof(out))
           ? 0
           : -(int64_t)EFAULT;
}

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

int64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
  switch (option) {
  case PR_GET_DUMPABLE:
    return 1;
  case PR_SET_DUMPABLE:
    return arg2 <= 1 && arg3 == 0 && arg4 == 0 && arg5 == 0 ? 0 : -(int64_t)EINVAL;
  case PR_SET_NAME: {
    char name[16];
    if (arg2 == 0) { return -(int64_t)EFAULT; }
    for (size_t i = 0; i < sizeof(name); ++i) {
      if (!syscall_user_readable(arg2 + i, 1) || !vmm_copy_from_user(syscall_active_as(), &name[i], arg2 + i, 1)) {
        return -(int64_t)EFAULT;
      }
      if (name[i] == '\0') {
        cell_set_current_name(name);
        return 0;
      }
    }
    name[sizeof(name) - 1] = '\0';
    cell_set_current_name(name);
    return 0;
  }
  case PR_GET_NAME: {
    char name[16];
    const char *current = cell_current_name();
    size_t i = 0;
    for (; i + 1 < sizeof(name) && current[i] != '\0'; ++i) {
      name[i] = current[i];
    }
    name[i] = '\0';
    return syscall_user_writable(arg2, sizeof(name)) && vmm_copy_to_user(syscall_active_as(), arg2, name, sizeof(name))
             ? 0
             : -(int64_t)EFAULT;
  }
  case PR_SET_NO_NEW_PRIVS:
    return arg2 == 1 && arg3 == 0 && arg4 == 0 && arg5 == 0 ? 0 : -(int64_t)EINVAL;
  case PR_GET_NO_NEW_PRIVS:
    return 0;
  case PR_SET_VMA:
    return arg2 == PR_SET_VMA_ANON_NAME ? 0 : -(int64_t)EINVAL;
  default:
    kprintf("[kernel] prctl option=%d unsupported pid=%d name=%s\n", (int)option, cell_current_pid(),
            cell_current_name());
    return -(int64_t)EINVAL;
  }
}

int64_t sys_spore_net_config(uint64_t op, uint64_t cfg_addr) {
  if (cfg_addr == 0) { return -(int64_t)EFAULT; }
  if (op == 0) {
    struct net_config cfg;
    net_get_config(&cfg);
    struct net_config64 out = {
      .local_ip = cfg.local_ip,
      .gateway_ip = cfg.gateway_ip,
      .netmask = cfg.netmask,
      .dns_ip = cfg.dns_ip,
      .configured = cfg.configured,
    };
    return syscall_user_writable(cfg_addr, sizeof(out)) &&
               vmm_copy_to_user(syscall_active_as(), cfg_addr, &out, sizeof(out))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (op == 1) {
    if (cell_current_euid() != 0) { return -(int64_t)EPERM; }
    struct net_config64 in;
    if (!syscall_user_readable(cfg_addr, sizeof(in)) ||
        !vmm_copy_from_user(syscall_active_as(), &in, cfg_addr, sizeof(in))) {
      return -(int64_t)EFAULT;
    }
    struct net_config cfg = {
      .local_ip = in.local_ip,
      .gateway_ip = in.gateway_ip,
      .netmask = in.netmask,
      .dns_ip = in.dns_ip,
      .configured = in.configured != 0,
    };
    net_set_config(&cfg);
    return 0;
  }
  return -(int64_t)EINVAL;
}

int64_t sys_sched_getaffinity(uint64_t mask, uint64_t len) {
  enum { AFFINITY_WORDS = (SPORE_BOOT_CPU_MAX + 63u) / 64u };
  uint32_t possible = smp_possible_cpu_count();
  if (possible == 0) { possible = 1; }
  if (possible > SPORE_BOOT_CPU_MAX) { possible = SPORE_BOOT_CPU_MAX; }
  size_t bytes = ((size_t)(possible + 63u) / 64u) * sizeof(uint64_t);
  if (len < bytes || !syscall_user_writable(mask, bytes)) { return -(int64_t)EINVAL; }

  uint64_t out[AFFINITY_WORDS];
  kmemset(out, 0, sizeof(out));
  for (uint32_t cpu = 0; cpu < possible; ++cpu) {
    if (smp_cpu_online(cpu)) {
      out[cpu / 64u] |= 1ull << (cpu % 64u);
    }
  }
  return vmm_copy_to_user(syscall_active_as(), mask, out, bytes) ? (int64_t)bytes : -(int64_t)EFAULT;
}
