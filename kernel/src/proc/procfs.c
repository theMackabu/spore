#include "proc/procfs.h"

#include "ext2.h"
#include "kprintf.h"
#include "kstr.h"
#include "mem.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "ramfs.h"
#include "virtio_net.h"
#include "vfs.h"

#include <stddef.h>

static uint64_t proc_cache_tick = UINT64_MAX;
static size_t proc_cache_rss[MAX_DOMAINS];

static size_t domain_resident_pages(const struct domain *domain);

static const char *thread_state_text(enum thread_state state) {
  switch (state) {
  case THREAD_RUNNABLE:
    return "running";
  case THREAD_BLOCKED:
    return "blocked";
  case THREAD_ZOMBIE:
    return "zombie";
  case THREAD_UNUSED:
    return "unused";
  }
  return "unknown";
}

static const char *wait_reason_text(enum wait_reason reason) {
  switch (reason) {
  case WAIT_NONE:
    return "-";
  case WAIT_CHILD:
    return "child";
  case WAIT_STDIN:
    return "stdin";
  case WAIT_SOCKET:
    return "socket";
  case WAIT_THREAD:
    return "thread";
  case WAIT_FUTEX:
    return "futex";
  case WAIT_POLL:
    return "poll";
  case WAIT_SLEEP:
    return "sleep";
  case WAIT_VFORK:
    return "vfork";
  case WAIT_PIPE:
    return "pipe";
  case WAIT_EPOLL:
    return "epoll";
  }
  return "?";
}

static const char *domain_state_text(const struct domain *domain) {
  if (domain == NULL) { return "unknown"; }
  if (domain->zombie) { return "zombie"; }
  bool blocked = false;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->domain != domain || thread->state == THREAD_UNUSED) { continue; }
    if (thread->state == THREAD_RUNNABLE) { return "running"; }
    if (thread->state == THREAD_BLOCKED) { blocked = true; }
  }
  return blocked ? "blocked" : "unknown";
}

static char domain_proc_state_char(const struct domain *domain) {
  const char *state = domain_state_text(domain);
  if (state[0] == 'r') { return 'R'; }
  if (state[0] == 'b') { return 'S'; }
  if (state[0] == 'z') { return 'Z'; }
  return '?';
}

static const char *domain_wait_text(const struct domain *domain) {
  if (domain == NULL || domain->zombie) { return "-"; }
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain && thread->state == THREAD_BLOCKED) {
      return wait_reason_text(thread->wait_reason);
    }
  }
  return "-";
}

static void proc_append_char(char *dst, size_t cap, size_t *len, char c) {
  if (*len + 1 < cap) {
    dst[*len] = c;
    ++*len;
    dst[*len] = '\0';
  }
}

static void proc_append_str(char *dst, size_t cap, size_t *len, const char *s) {
  while (*s != '\0') {
    proc_append_char(dst, cap, len, *s++);
  }
}

static void proc_append_u64(char *dst, size_t cap, size_t *len, uint64_t value) {
  char tmp[32];
  size_t n = 0;
  if (value == 0) {
    proc_append_char(dst, cap, len, '0');
    return;
  }
  while (value != 0 && n < sizeof(tmp)) {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  }
  while (n > 0) {
    proc_append_char(dst, cap, len, tmp[--n]);
  }
}

static void proc_append_u64_pad(char *dst, size_t cap, size_t *len, uint64_t value, size_t width) {
  char tmp[32];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  } while (value != 0 && n < sizeof(tmp));
  while (n < width && n < sizeof(tmp)) {
    tmp[n++] = '0';
  }
  while (n > 0) {
    proc_append_char(dst, cap, len, tmp[--n]);
  }
}

static void proc_append_hex(char *dst, size_t cap, size_t *len, uint64_t value, size_t digits) {
  static const char hex[] = "0123456789abcdef";
  proc_append_str(dst, cap, len, "0x");
  for (size_t i = 0; i < digits; ++i) {
    size_t shift = (digits - i - 1) * 4;
    proc_append_char(dst, cap, len, hex[(value >> shift) & 0xf]);
  }
}

static size_t procinfo_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "pid ppid state wait rss_pages cpu_ticks age_ticks budget_remaining budget_max name exec_path cwd "
                  "cmdline unsupported_syscalls last_unsupported_syscall\n");
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    const struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used) { continue; }
    proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain_state_text(domain));
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain_wait_text(domain));
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain_resident_pages(domain));
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->cpu_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, cell_uptime_ticks() - domain->start_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->budget.remaining_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->budget.max_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->name);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? "-" : domain->exec_path);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->cwd);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->cmdline[0] == '\0' ? domain->name : domain->cmdline);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->unsupported_syscalls);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->last_unsupported_syscall);
    proc_append_char(dst, cap, &len, '\n');
  }
  return len;
}

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

static size_t meminfo_text(char *dst, size_t cap) {
  size_t len = 0;
  uint64_t total_pages = pmm_total_pages();
  uint64_t free_pages = pmm_free_pages();
  uint64_t used_pages = total_pages > free_pages ? total_pages - free_pages : 0;
  uint64_t buffer_pages = ext2_cache_used_pages();
  uint64_t shmem_pages = ramfs_backing_used_pages();
  uint64_t reclaimable_pages = buffer_pages;
  uint64_t inactive_pages = reclaimable_pages + shmem_pages;
  uint64_t active_pages = used_pages > inactive_pages ? used_pages - inactive_pages : 0;
  uint64_t total_kib = total_pages * 4;
  uint64_t free_kib = free_pages * 4;
  uint64_t used_kib = used_pages * 4;
  uint64_t buffer_kib = buffer_pages * 4;
  uint64_t cached_kib = 0;
  uint64_t shmem_kib = shmem_pages * 4;
  uint64_t reclaimable_kib = reclaimable_pages * 4;
  uint64_t active_kib = active_pages * 4;
  uint64_t inactive_kib = inactive_pages * 4;
  uint64_t available_kib = free_kib + reclaimable_kib;
  proc_append_str(dst, cap, &len, "MemTotal: ");
  proc_append_u64(dst, cap, &len, total_kib);
  proc_append_str(dst, cap, &len, " kB\nMemFree: ");
  proc_append_u64(dst, cap, &len, free_kib);
  proc_append_str(dst, cap, &len, " kB\nMemAvailable: ");
  proc_append_u64(dst, cap, &len, available_kib);
  proc_append_str(dst, cap, &len, " kB\nBuffers: ");
  proc_append_u64(dst, cap, &len, buffer_kib);
  proc_append_str(dst, cap, &len, " kB\nCached: ");
  proc_append_u64(dst, cap, &len, cached_kib);
  proc_append_str(dst, cap, &len, " kB\nSwapCached: 0 kB\nActive: ");
  proc_append_u64(dst, cap, &len, active_kib);
  proc_append_str(dst, cap, &len, " kB\nInactive: ");
  proc_append_u64(dst, cap, &len, inactive_kib);
  proc_append_str(dst, cap, &len, " kB\nShmem: ");
  proc_append_u64(dst, cap, &len, shmem_kib);
  proc_append_str(dst, cap, &len, " kB\nSReclaimable: ");
  proc_append_u64(dst, cap, &len, reclaimable_kib);
  proc_append_str(dst, cap, &len, " kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
  proc_append_str(dst, cap, &len, "MemTotalPages: ");
  proc_append_u64(dst, cap, &len, total_pages);
  proc_append_str(dst, cap, &len, "\nMemFreePages: ");
  proc_append_u64(dst, cap, &len, free_pages);
  proc_append_str(dst, cap, &len, "\nMemUsedPages: ");
  proc_append_u64(dst, cap, &len, used_pages);
  proc_append_str(dst, cap, &len, "\nMemTotalKiB: ");
  proc_append_u64(dst, cap, &len, total_kib);
  proc_append_str(dst, cap, &len, "\nMemUsedKiB: ");
  proc_append_u64(dst, cap, &len, used_kib);
  proc_append_str(dst, cap, &len, "\nMemFreeKiB: ");
  proc_append_u64(dst, cap, &len, free_kib);
  struct pmm_stats stats = pmm_get_stats();
  proc_append_str(dst, cap, &len, "\nPMMAllocAttempts: ");
  proc_append_u64(dst, cap, &len, stats.alloc_attempts);
  proc_append_str(dst, cap, &len, "\nPMMAllocSuccesses: ");
  proc_append_u64(dst, cap, &len, stats.alloc_successes);
  proc_append_str(dst, cap, &len, "\nPMMAllocFailures: ");
  proc_append_u64(dst, cap, &len, stats.alloc_failures);
  proc_append_str(dst, cap, &len, "\nPMMBitmapWordsScanned: ");
  proc_append_u64(dst, cap, &len, stats.bitmap_words_scanned);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t cpuinfo_text(char *dst, size_t cap) {
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

  proc_append_str(dst, cap, &len, "processor\t: 0\nvendor_id\t: ");
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
  proc_append_str(dst, cap, &len, "\ncpu MHz\t\t: ");
  proc_append_u64(dst, cap, &len, cntfrq / 1000000);
  proc_append_char(dst, cap, &len, '.');
  proc_append_u64_pad(dst, cap, &len, (cntfrq % 1000000) / 1000, 3);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t uptime_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_u64(dst, cap, &len, cell_uptime_ticks() / 100);
  proc_append_char(dst, cap, &len, '.');
  if ((cell_uptime_ticks() % 100) < 10) { proc_append_char(dst, cap, &len, '0'); }
  proc_append_u64(dst, cap, &len, cell_uptime_ticks() % 100);
  proc_append_str(dst, cap, &len, " 0.00\n");
  return len;
}

static size_t loadavg_text(char *dst, size_t cap) {
  size_t len = 0;
  uint64_t runnable = 0;
  uint64_t total = 0;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->domain == NULL) { continue; }
    ++total;
    if (thread->state == THREAD_RUNNABLE) { ++runnable; }
  }
  proc_append_str(dst, cap, &len, "0.00 0.00 0.00 ");
  proc_append_u64(dst, cap, &len, runnable);
  proc_append_char(dst, cap, &len, '/');
  proc_append_u64(dst, cap, &len, total);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, cell_domain_last_id());
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t mounts_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "ext2-root / ext2 rw 0 0\n"
                  "tmpfs /tmp tmpfs rw 0 0\n"
                  "runfs /run tmpfs rw 0 0\n"
                  "bootfs /dev/fs/boot fat16 ro 0 0\n"
                  "ramfs /dev/fs/ram0 ramfs ro 0 0\n"
                  "proc /proc proc ro 0 0\n"
                  "dev /dev devfs rw 0 0\n");
  return len;
}

static size_t net_dev_text(char *dst, size_t cap) {
  size_t len = 0;
  struct virtio_net_stats stats = virtio_net_stats();
  proc_append_str(dst, cap, &len,
                  "Inter-|   Receive                                                |  Transmit\n"
                  " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo "
                  "colls carrier compressed\n"
                  "  eth0: ");
  proc_append_u64(dst, cap, &len, stats.rx_bytes);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, stats.rx_packets);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0 ");
  proc_append_u64(dst, cap, &len, stats.tx_bytes);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, stats.tx_packets);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0\n");
  return len;
}

static size_t filesystems_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "nodev\tproc\n"
                  "nodev\tdevfs\n"
                  "nodev\ttmpfs\n"
                  "nodev\tramfs\n"
                  "\text2\n");
  return len;
}

static size_t partitions_text(char *dst, size_t cap) {
  size_t len = 0;
  struct vfs_fs_info info;
  uint64_t root_blocks = 0;
  if (vfs_fs_info(&info) && info.block_size != 0) { root_blocks = (info.block_count * info.block_size) / 1024; }
  proc_append_str(dst, cap, &len,
                  "major minor  #blocks  name\n"
                  " 254     0   ");
  proc_append_u64(dst, cap, &len, root_blocks);
  proc_append_str(dst, cap, &len,
                  "  vda\n"
                  " 254     1    16384  vdb\n");
  return len;
}

static size_t devices_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "Character devices:\n"
                  "  1 mem\n"
                  "  5 tty\n"
                  " 10 misc\n"
                  "\nBlock devices:\n"
                  "254 virtblk\n");
  return len;
}

static size_t kmsg_text(char *dst, size_t cap) {
  return (size_t)klog_copy(dst, cap);
}

static size_t fs_root_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "ext2 root filesystem mounted at /\n");
  return len;
}

static size_t fs_boot_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "ESP boot filesystem image used by UEFI\n");
  return len;
}

static size_t fs_ram0_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "synthetic boot ramfs for devfs/procfs and fallback modules\n");
  return len;
}

static size_t fs_tmp_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "tmpfs mounted at /tmp\n");
  return len;
}

static uint64_t total_domain_cpu_ticks(void) {
  uint64_t ticks = 0;
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain != NULL && domain->used) { ticks += domain->cpu_ticks; }
  }
  return ticks;
}

static size_t stat_text(char *dst, size_t cap) {
  size_t len = 0;
  uint64_t idle_ticks = cell_idle_ticks() > cell_uptime_ticks() ? cell_uptime_ticks() : cell_idle_ticks();
  uint64_t busy_ticks = cell_uptime_ticks() - idle_ticks;
  uint64_t running = 0;
  uint64_t blocked = 0;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->state == THREAD_RUNNABLE) {
      ++running;
    } else if (thread != NULL && thread->state == THREAD_BLOCKED) {
      ++blocked;
    }
  }
  proc_append_str(dst, cap, &len, "cpu  ");
  proc_append_u64(dst, cap, &len, busy_ticks);
  proc_append_str(dst, cap, &len, " 0 0 ");
  proc_append_u64(dst, cap, &len, idle_ticks);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0\ncpu0 ");
  proc_append_u64(dst, cap, &len, busy_ticks);
  proc_append_str(dst, cap, &len, " 0 0 ");
  proc_append_u64(dst, cap, &len, idle_ticks);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0\nctxt ");
  proc_append_u64(dst, cap, &len, cell_uptime_ticks());
  proc_append_str(dst, cap, &len, "\nbtime ");
  proc_append_u64(dst, cap, &len, cell_boot_epoch_seconds());
  proc_append_str(dst, cap, &len, "\nprocesses ");
  proc_append_u64(dst, cap, &len, cell_domain_last_id());
  proc_append_str(dst, cap, &len, "\nprocs_running ");
  proc_append_u64(dst, cap, &len, running == 0 ? 1 : running);
  proc_append_str(dst, cap, &len, "\nprocs_blocked ");
  proc_append_u64(dst, cap, &len, blocked);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static const struct domain *domain_for_pid(int pid) {
  return cell_find_domain(pid);
}

static size_t proc_pid_status_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, "Name:\t");
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_str(dst, cap, &len, "\nState:\t");
  proc_append_str(dst, cap, &len, domain_state_text(domain));
  proc_append_str(dst, cap, &len, "\nPid:\t");
  proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
  proc_append_str(dst, cap, &len, "\nPPid:\t");
  proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
  proc_append_str(dst, cap, &len, "\nUid:\t");
  proc_append_u64(dst, cap, &len, domain->uid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->euid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->uid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->euid);
  proc_append_str(dst, cap, &len, "\nGid:\t");
  proc_append_u64(dst, cap, &len, domain->gid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->egid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->gid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->egid);
  proc_append_str(dst, cap, &len, "\nVmRSSPages:\t");
  proc_append_u64(dst, cap, &len, domain_resident_pages(domain));
  proc_append_str(dst, cap, &len, "\nCpuTicks:\t");
  proc_append_u64(dst, cap, &len, domain->cpu_ticks);
  proc_append_str(dst, cap, &len, "\nCwd:\t");
  proc_append_str(dst, cap, &len, domain->cwd);
  proc_append_str(dst, cap, &len, "\nUnsupportedSyscalls:\t");
  proc_append_u64(dst, cap, &len, domain->unsupported_syscalls);
  proc_append_str(dst, cap, &len, "\nLastUnsupportedSyscall:\t");
  proc_append_u64(dst, cap, &len, domain->last_unsupported_syscall);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_stat_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  uint64_t rss = domain_resident_pages(domain);
  uint64_t start_time = domain->start_ticks;
  uint64_t utime = domain->cpu_ticks;
  proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
  proc_append_str(dst, cap, &len, " (");
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_str(dst, cap, &len, ") ");
  proc_append_char(dst, cap, &len, domain_proc_state_char(domain));
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0 0 0 0 0 ");
  proc_append_u64(dst, cap, &len, utime);
  proc_append_str(dst, cap, &len, " 0 0 0 20 0 1 0 ");
  proc_append_u64(dst, cap, &len, start_time);
  proc_append_str(dst, cap, &len, " ");
  proc_append_u64(dst, cap, &len, rss * PAGE_SIZE);
  proc_append_str(dst, cap, &len, " ");
  proc_append_u64(dst, cap, &len, rss);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
  return len;
}

static size_t proc_pid_cmdline_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? domain->name : domain->exec_path);
  proc_append_char(dst, cap, &len, '\0');
  return len;
}

static size_t proc_pid_statm_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  uint64_t rss = domain_resident_pages(domain);
  uint64_t virt = rss == 0 ? 1 : rss;
  proc_append_u64(dst, cap, &len, virt);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, rss);
  proc_append_str(dst, cap, &len, " 0 0 0 ");
  proc_append_u64(dst, cap, &len, rss);
  proc_append_str(dst, cap, &len, " 0\n");
  return len;
}

static size_t proc_pid_comm_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_cwd_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->cwd);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_exe_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? "-" : domain->exec_path);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static int64_t read_generated_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len,
                                     size_t (*fill)(char *, size_t)) {
  char text[2048] = {0};
  size_t text_len = fill(text, sizeof(text));
  if (file->offset >= text_len) { return 0; }
  size_t chunk = text_len - (size_t)file->offset;
  if (chunk > len) { chunk = (size_t)len; }
  if (!vmm_copy_to_user(&domain->as, buf, text + file->offset, chunk)) { return -14; }
  file->offset += chunk;
  return (int64_t)chunk;
}

static int64_t read_generated_pid_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len,
                                         size_t (*fill)(char *, size_t, int)) {
  char text[2048] = {0};
  size_t text_len = fill(text, sizeof(text), file->node.proc_pid);
  if (file->offset >= text_len) { return 0; }
  size_t chunk = text_len - (size_t)file->offset;
  if (chunk > len) { chunk = (size_t)len; }
  if (!vmm_copy_to_user(&domain->as, buf, text + file->offset, chunk)) { return -14; }
  file->offset += chunk;
  return (int64_t)chunk;
}

static size_t domain_resident_pages_uncached(const struct domain *domain) {
  size_t pages = 0;
  for (size_t i = 0; i < vma_capacity(&domain->vmas); ++i) {
    const struct vma *vma = vma_at(&domain->vmas, i);
    if (vma->used) { pages += vmm_mapped_pages_in_range(&domain->as, vma->start, vma->end); }
  }
  return pages;
}

static void refresh_proc_cache(void) {
  if (proc_cache_tick == cell_uptime_ticks()) { return; }
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    struct domain *domain = cell_domain_slot(i);
    proc_cache_rss[i] = domain != NULL && domain->used ? domain_resident_pages_uncached(domain) : 0;
  }
  proc_cache_tick = cell_uptime_ticks();
}

static size_t domain_resident_pages(const struct domain *domain) {
  if (domain == NULL) { return 0; }
  refresh_proc_cache();
  size_t index = cell_domain_index(domain);
  return index < MAX_DOMAINS ? proc_cache_rss[index] : 0;
}

size_t cell_proc_info(struct proc_info *out, size_t max) {
  size_t count = 0;
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    const struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used) { continue; }
    if (count < max && out != NULL) {
      struct proc_info info = {
        .pid = (uint32_t)domain->id,
        .tid = (uint32_t)(cell_thread_for_domain((struct domain *)domain) == NULL
                            ? 0
                            : cell_thread_for_domain((struct domain *)domain)->tid),
        .ppid = (uint32_t)domain->parent_id,
        .state = (uint32_t)(domain->zombie
                              ? THREAD_ZOMBIE
                              : (str_eq(domain_state_text(domain), "blocked") ? THREAD_BLOCKED : THREAD_RUNNABLE)),
        .wait_reason = 0,
        .resident_pages = domain_resident_pages(domain),
        .cpu_ticks = domain->cpu_ticks,
        .start_ticks = domain->start_ticks,
        .remaining_ticks = domain->budget.remaining_ticks,
        .max_ticks = domain->budget.max_ticks,
      };
      copy_cstr(info.name, sizeof(info.name), domain->name);
      copy_cstr(info.exec_path, sizeof(info.exec_path), domain->exec_path);
      copy_cstr(info.argv0, sizeof(info.argv0), domain->argv0);
      copy_cstr(info.cmdline, sizeof(info.cmdline), domain->cmdline);
      copy_cstr(info.cwd, sizeof(info.cwd), domain->cwd);
      out[count] = info;
    }
    ++count;
  }
  return count;
}

int64_t cell_procfs_read_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len) {
  switch (file->node.device) {
  case RAMFS_DEV_PROCINFO:
    return read_generated_device(file, domain, buf, len, procinfo_text);
  case RAMFS_DEV_MEMINFO:
    return read_generated_device(file, domain, buf, len, meminfo_text);
  case RAMFS_DEV_CPUINFO:
    return read_generated_device(file, domain, buf, len, cpuinfo_text);
  case RAMFS_DEV_UPTIME:
    return read_generated_device(file, domain, buf, len, uptime_text);
  case RAMFS_DEV_LOADAVG:
    return read_generated_device(file, domain, buf, len, loadavg_text);
  case RAMFS_DEV_MOUNTS:
    return read_generated_device(file, domain, buf, len, mounts_text);
  case RAMFS_DEV_STAT:
    return read_generated_device(file, domain, buf, len, stat_text);
  case RAMFS_DEV_NET_DEV:
    return read_generated_device(file, domain, buf, len, net_dev_text);
  case RAMFS_DEV_KMSG:
    return read_generated_device(file, domain, buf, len, kmsg_text);
  case RAMFS_DEV_FILESYSTEMS:
    return read_generated_device(file, domain, buf, len, filesystems_text);
  case RAMFS_DEV_PARTITIONS:
    return read_generated_device(file, domain, buf, len, partitions_text);
  case RAMFS_DEV_DEVICES:
    return read_generated_device(file, domain, buf, len, devices_text);
  case RAMFS_DEV_PROC_PID_STAT:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_stat_text);
  case RAMFS_DEV_PROC_PID_STATUS:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_status_text);
  case RAMFS_DEV_PROC_PID_CMDLINE:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_cmdline_text);
  case RAMFS_DEV_PROC_PID_STATM:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_statm_text);
  case RAMFS_DEV_PROC_PID_COMM:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_comm_text);
  case RAMFS_DEV_PROC_PID_MOUNTS:
    return read_generated_device(file, domain, buf, len, mounts_text);
  case RAMFS_DEV_PROC_PID_CWD:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_cwd_text);
  case RAMFS_DEV_PROC_PID_EXE:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_exe_text);
  case RAMFS_DEV_FS_ROOT:
    return read_generated_device(file, domain, buf, len, fs_root_text);
  case RAMFS_DEV_FS_BOOT:
    return read_generated_device(file, domain, buf, len, fs_boot_text);
  case RAMFS_DEV_FS_RAM0:
    return read_generated_device(file, domain, buf, len, fs_ram0_text);
  case RAMFS_DEV_FS_TMP:
    return read_generated_device(file, domain, buf, len, fs_tmp_text);
  default:
    return -22;
  }
}
