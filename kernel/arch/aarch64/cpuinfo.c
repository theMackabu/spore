#include "arch/aarch64/cpuinfo.h"

#include "arch/aarch64/smp.h"
#include "fs/procfs_format.h"

#include <stdbool.h>
#include <stdint.h>

static const char *cpu_implementer_name(uint64_t implementer) {
  switch (implementer) {
  case 0x41:
    return "ARM";
  case 0x42:
    return "Broadcom";
  case 0x43:
    return "Cavium";
  case 0x46:
    return "Fujitsu";
  case 0x48:
    return "HiSilicon";
  case 0x4e:
    return "NVIDIA";
  case 0x50:
    return "AppliedMicro";
  case 0x51:
    return "Qualcomm";
  case 0x53:
    return "Samsung";
  case 0x56:
    return "Marvell";
  case 0x61:
    return "Apple";
  case 0x69:
    return "Intel";
  default:
    return "Unknown";
  }
}

static const char *arm_cpu_part_name(uint64_t part) {
  switch (part) {
  case 0xd03:
    return "ARM Cortex-A53";
  case 0xd04:
    return "ARM Cortex-A35";
  case 0xd05:
    return "ARM Cortex-A55";
  case 0xd07:
    return "ARM Cortex-A57";
  case 0xd08:
    return "ARM Cortex-A72";
  case 0xd09:
    return "ARM Cortex-A73";
  case 0xd0a:
    return "ARM Cortex-A75";
  case 0xd0b:
    return "ARM Cortex-A76";
  case 0xd0c:
    return "ARM Neoverse-N1";
  case 0xd40:
    return "ARM Neoverse-V1";
  case 0xd41:
    return "ARM Cortex-A78";
  case 0xd44:
    return "ARM Cortex-X1";
  case 0xd46:
    return "ARM Cortex-A510";
  case 0xd47:
    return "ARM Cortex-A710";
  case 0xd48:
    return "ARM Cortex-X2";
  case 0xd49:
    return "ARM Neoverse-N2";
  case 0xd4a:
    return "ARM Neoverse-E1";
  case 0xd4b:
    return "ARM Cortex-A78C";
  case 0xd4d:
    return "ARM Cortex-A715";
  case 0xd4e:
    return "ARM Cortex-X3";
  case 0xd4f:
    return "ARM Neoverse-V2";
  case 0xd80:
    return "ARM Cortex-A520";
  case 0xd81:
    return "ARM Cortex-A720";
  case 0xd82:
    return "ARM Cortex-X4";
  default:
    return "ARM AArch64 CPU";
  }
}

static const char *cpu_model_name(uint64_t implementer, uint64_t part) {
  if (implementer == 0x41) { return arm_cpu_part_name(part); }
  if (implementer == 0x61) { return "Apple AArch64 CPU"; }
  return "AArch64 CPU";
}

static void proc_append_cpu_features(char *dst, size_t cap, size_t *len, uint64_t pfr0, uint64_t isar0) {
  bool first = true;
#define APPEND_FEATURE(name)                                                                                           \
  do {                                                                                                                 \
    if (!first) { proc_append_char(dst, cap, len, ' '); }                                                              \
    proc_append_str(dst, cap, len, name);                                                                              \
    first = false;                                                                                                     \
  } while (0)
  uint64_t fp = (pfr0 >> 16) & 0xf;
  uint64_t asimd = (pfr0 >> 20) & 0xf;
  uint64_t aes = (isar0 >> 4) & 0xf;
  uint64_t sha1 = (isar0 >> 8) & 0xf;
  uint64_t sha2 = (isar0 >> 12) & 0xf;
  uint64_t crc32 = (isar0 >> 16) & 0xf;
  uint64_t atomic = (isar0 >> 20) & 0xf;
  uint64_t rndr = (isar0 >> 60) & 0xf;
  if (fp != 0xf) { APPEND_FEATURE("fp"); }
  if (asimd != 0xf) { APPEND_FEATURE("asimd"); }
  if (aes >= 1) { APPEND_FEATURE("aes"); }
  if (aes >= 2) { APPEND_FEATURE("pmull"); }
  if (sha1 >= 1) { APPEND_FEATURE("sha1"); }
  if (sha2 >= 1) { APPEND_FEATURE("sha2"); }
  if (crc32 >= 1) { APPEND_FEATURE("crc32"); }
  if (atomic >= 2) { APPEND_FEATURE("atomics"); }
  if (rndr >= 1) { APPEND_FEATURE("rng"); }
  if (first) { proc_append_str(dst, cap, len, "none"); }
#undef APPEND_FEATURE
}

size_t proc_cpuinfo_text(char *dst, size_t cap) {
  size_t len = 0;
  uint64_t midr = 0;
  uint64_t pfr0 = 0;
  uint64_t isar0 = 0;
  uint64_t cntfrq = 0;
  __asm__ volatile("mrs %0, MIDR_EL1" : "=r"(midr));
  __asm__ volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(pfr0));
  __asm__ volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));
  __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(cntfrq));

  uint64_t implementer = (midr >> 24) & 0xff;
  uint64_t variant = (midr >> 20) & 0xf;
  uint64_t architecture = (midr >> 16) & 0xf;
  uint64_t linux_architecture = architecture == 0xf ? 8 : architecture;
  uint64_t part = (midr >> 4) & 0xfff;
  uint64_t revision = midr & 0xf;
  const char *model = cpu_model_name(implementer, part);

  uint32_t online = smp_online_cpu_count();
  if (online == 0) { online = 1; }
  for (uint32_t cpu = 0; cpu < online; ++cpu) {
    proc_append_str(dst, cap, &len, "processor\t: ");
    proc_append_u64(dst, cap, &len, cpu);
    proc_append_str(dst, cap, &len, "\nvendor_id\t: ");
    proc_append_str(dst, cap, &len, cpu_implementer_name(implementer));
    proc_append_str(dst, cap, &len, "\nmodel name\t: ");
    proc_append_str(dst, cap, &len, model);
    proc_append_str(dst, cap, &len, "\nBogoMIPS\t: ");
    proc_append_u64(dst, cap, &len, cntfrq / 500000);
    proc_append_str(dst, cap, &len, ".00\nFeatures\t: ");
    proc_append_cpu_features(dst, cap, &len, pfr0, isar0);
    proc_append_str(dst, cap, &len, "\nCPU implementer\t: ");
    proc_append_hex(dst, cap, &len, implementer, 2);
    proc_append_str(dst, cap, &len, "\nCPU architecture: ");
    proc_append_u64(dst, cap, &len, linux_architecture);
    proc_append_str(dst, cap, &len, "\nCPU variant\t: ");
    proc_append_hex(dst, cap, &len, variant, 1);
    proc_append_str(dst, cap, &len, "\nCPU part\t: ");
    proc_append_hex(dst, cap, &len, part, 3);
    proc_append_str(dst, cap, &len, "\nCPU revision\t: ");
    proc_append_u64(dst, cap, &len, revision);
    proc_append_str(dst, cap, &len, "\nHardware\t: ");
    proc_append_str(dst, cap, &len, model);
    proc_append_str(dst, cap, &len, "\nMPIDR\t\t: ");
    proc_append_hex(dst, cap, &len, smp_cpu_mpidr(cpu), 8);
    proc_append_str(dst, cap, &len, "\nonline\t\t: ");
    proc_append_u64(dst, cap, &len, smp_cpu_online(cpu) ? 1 : 0);
    proc_append_str(dst, cap, &len, "\ncpu MHz\t\t: ");
    proc_append_u64(dst, cap, &len, cntfrq / 1000000);
    proc_append_char(dst, cap, &len, '.');
    proc_append_u64_pad(dst, cap, &len, (cntfrq % 1000000) / 1000, 3);
    proc_append_str(dst, cap, &len, "\n\n");
  }
  return len;
}
