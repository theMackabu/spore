#include "proc/procfs_text.h"

#include "cell.h"
#include "ext2.h"
#include "kprintf.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/procfs_format.h"
#include "proc/thread.h"
#include "ramfs.h"
#include "virtio_net.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
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

size_t proc_meminfo_text(char *dst, size_t cap) {
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

size_t proc_uptime_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_u64(dst, cap, &len, cell_uptime_ticks() / 100);
  proc_append_char(dst, cap, &len, '.');
  if ((cell_uptime_ticks() % 100) < 10) { proc_append_char(dst, cap, &len, '0'); }
  proc_append_u64(dst, cap, &len, cell_uptime_ticks() % 100);
  proc_append_str(dst, cap, &len, " 0.00\n");
  return len;
}

size_t proc_loadavg_text(char *dst, size_t cap) {
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

size_t proc_mounts_text(char *dst, size_t cap) {
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

size_t proc_net_dev_text(char *dst, size_t cap) {
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

size_t proc_filesystems_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "nodev\tproc\n"
                  "nodev\tdevfs\n"
                  "nodev\ttmpfs\n"
                  "nodev\tramfs\n"
                  "\text2\n");
  return len;
}

size_t proc_partitions_text(char *dst, size_t cap) {
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

size_t proc_devices_text(char *dst, size_t cap) {
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

size_t proc_fsstats_text(char *dst, size_t cap) {
  size_t len = 0;
  struct vfs_stats vfs = vfs_get_stats();
  struct ext2_stats ext2 = ext2_get_stats();
  proc_append_str(dst, cap, &len, "vfs.lookup ");
  proc_append_u64(dst, cap, &len, vfs.lookup_count);
  proc_append_str(dst, cap, &len, "\nvfs.lstat ");
  proc_append_u64(dst, cap, &len, vfs.lstat_count);
  proc_append_str(dst, cap, &len, "\nvfs.lookup_cache_hits ");
  proc_append_u64(dst, cap, &len, vfs.lookup_cache_hits);
  proc_append_str(dst, cap, &len, "\nvfs.lookup_cache_misses ");
  proc_append_u64(dst, cap, &len, vfs.lookup_cache_misses);
  proc_append_str(dst, cap, &len, "\nvfs.lookup_cache_invalidations ");
  proc_append_u64(dst, cap, &len, vfs.lookup_cache_invalidations);
  proc_append_str(dst, cap, &len, "\nvfs.dirent ");
  proc_append_u64(dst, cap, &len, vfs.dirent_count);
  proc_append_str(dst, cap, &len, "\nvfs.next_dirent ");
  proc_append_u64(dst, cap, &len, vfs.next_dirent_count);
  proc_append_str(dst, cap, &len, "\nvfs.page_cache_hits ");
  proc_append_u64(dst, cap, &len, vfs.page_cache_hits);
  proc_append_str(dst, cap, &len, "\nvfs.page_cache_misses ");
  proc_append_u64(dst, cap, &len, vfs.page_cache_misses);
  proc_append_str(dst, cap, &len, "\nvfs.page_cache_loads ");
  proc_append_u64(dst, cap, &len, vfs.page_cache_loads);
  proc_append_str(dst, cap, &len, "\nvfs.page_cache_invalidations ");
  proc_append_u64(dst, cap, &len, vfs.page_cache_invalidations);
  proc_append_str(dst, cap, &len, "\next2.lookup ");
  proc_append_u64(dst, cap, &len, ext2.lookup_count);
  proc_append_str(dst, cap, &len, "\next2.lstat ");
  proc_append_u64(dst, cap, &len, ext2.lstat_count);
  proc_append_str(dst, cap, &len, "\next2.lookup_child ");
  proc_append_u64(dst, cap, &len, ext2.lookup_child_count);
  proc_append_str(dst, cap, &len, "\next2.dir_iter ");
  proc_append_u64(dst, cap, &len, ext2.dir_iter_count);
  proc_append_str(dst, cap, &len, "\next2.block_cache_hits ");
  proc_append_u64(dst, cap, &len, ext2.block_cache_hits);
  proc_append_str(dst, cap, &len, "\next2.block_cache_misses ");
  proc_append_u64(dst, cap, &len, ext2.block_cache_misses);
  proc_append_str(dst, cap, &len, "\next2.block_cache_writes ");
  proc_append_u64(dst, cap, &len, ext2.block_cache_writes);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

size_t proc_kmsg_text(char *dst, size_t cap) {
  return (size_t)klog_copy(dst, cap);
}

size_t proc_fs_root_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "ext2 root filesystem mounted at /\n");
  return len;
}

size_t proc_fs_boot_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "ESP boot filesystem image used by UEFI\n");
  return len;
}

size_t proc_fs_ram0_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "synthetic boot ramfs for devfs/procfs and fallback modules\n");
  return len;
}

size_t proc_fs_tmp_text(char *dst, size_t cap) {
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

size_t proc_stat_text(char *dst, size_t cap) {
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
