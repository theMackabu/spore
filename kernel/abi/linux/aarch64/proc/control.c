#include "../usercopy.h"
#include "handlers.h"
#include "arch/aarch64/smp.h"
#include "proc/domain.h"
#include "lib/kprintf.h"
#include "lib/mem.h"

enum {
  PR_GET_DUMPABLE = 3,
  PR_SET_DUMPABLE = 4,
  PR_SET_NAME = 15,
  PR_GET_NAME = 16,
  PR_SET_NO_NEW_PRIVS = 38,
  PR_GET_NO_NEW_PRIVS = 39,
  PR_SET_VMA = 0x53564d41,
  PR_SET_VMA_ANON_NAME = 0,
  EFAULT = 14,
  EINVAL = 22,
};

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

int64_t sys_sched_getaffinity(uint64_t mask, uint64_t len) {
  constexpr int AFFINITY_WORDS = (SPORE_BOOT_CPU_MAX + 63u) / 64u;
  uint32_t possible = smp_possible_cpu_count();
  if (possible == 0) { possible = 1; }
  if (possible > SPORE_BOOT_CPU_MAX) { possible = SPORE_BOOT_CPU_MAX; }
  size_t bytes = ((size_t)(possible + 63u) / 64u) * sizeof(uint64_t);
  if (len < bytes || !syscall_user_writable(mask, bytes)) { return -(int64_t)EINVAL; }

  uint64_t out[AFFINITY_WORDS];
  kmemset(out, 0, sizeof(out));
  for (uint32_t cpu = 0; cpu < possible; ++cpu) {
    if (smp_cpu_online(cpu)) { out[cpu / 64u] |= 1ull << (cpu % 64u); }
  }
  return vmm_copy_to_user(syscall_active_as(), mask, out, bytes) ? (int64_t)bytes : -(int64_t)EFAULT;
}
