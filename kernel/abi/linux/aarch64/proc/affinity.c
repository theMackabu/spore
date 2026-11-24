#include "handlers.h"
#include "abi/linux/shared/usercopy.h"
#include "arch/aarch64/smp.h"
#include "lib/mem.h"

enum { EINVAL = 22, EFAULT = 14 };

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
