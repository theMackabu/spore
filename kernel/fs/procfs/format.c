#include "text.h"

#include "arch/aarch64/smp.h"
#include "proc/accounting.h"
#include "fs/ext2.h"
#include "lib/kprintf.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "fs/procfs_format.h"
#include "proc/thread.h"
#include "fs/ramfs.h"
#include "fs/vfs.h"
#include "drivers/virtio_net.h"

#include <stddef.h>
#include <stdint.h>

size_t proc_meminfo_text(char *dst, size_t cap) {
  size_t len = 0;
  uint64_t total_pages = pmm_total_pages();
  uint64_t free_pages = pmm_free_pages();
  uint64_t used_pages = total_pages > free_pages ? total_pages - free_pages : 0;
  uint64_t buffer_pages = ext2_cache_used_pages();
  uint64_t cached_pages = vfs_get_stats().page_cache_pages;
  uint64_t shmem_pages = ramfs_backing_used_pages();
  uint64_t reclaimable_pages = 0;
  uint64_t inactive_pages = buffer_pages + cached_pages + shmem_pages;
  uint64_t active_pages = used_pages > inactive_pages ? used_pages - inactive_pages : 0;
  uint64_t total_kib = total_pages * 4;
  uint64_t free_kib = free_pages * 4;
  uint64_t used_kib = active_pages * 4;
  uint64_t buffer_kib = buffer_pages * 4;
  uint64_t cached_kib = cached_pages * 4;
  uint64_t shmem_kib = shmem_pages * 4;
  uint64_t reclaimable_kib = reclaimable_pages * 4;
  uint64_t active_kib = active_pages * 4;
  uint64_t inactive_kib = inactive_pages * 4;
  uint64_t available_kib = free_kib + buffer_kib + cached_kib + reclaimable_kib;
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
  uint64_t avg[3] = {0, 0, 0};
  cell_loadavg_scaled(avg);
  for (size_t i = 0; i < cell_thread_capacity(); ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->domain == NULL) { continue; }
    ++total;
    if (thread->state == THREAD_RUNNABLE) { ++runnable; }
  }
  for (size_t i = 0; i < 3; ++i) {
    proc_append_u64(dst, cap, &len, avg[i] / 2048);
    proc_append_char(dst, cap, &len, '.');
    proc_append_u64_pad(dst, cap, &len, ((avg[i] % 2048) * 100) / 2048, 2);
    proc_append_char(dst, cap, &len, ' ');
  }
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
  const struct vfs_mount *m;
  for (size_t i = 0; (m = vfs_mount_at(i)) != NULL; ++i) {
    proc_append_str(dst, cap, &len, m->source);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, m->target);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, m->fstype);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, m->options);
    proc_append_str(dst, cap, &len, " 0 0\n");
  }
  return len;
}

size_t proc_mountinfo_text(char *dst, size_t cap) {
  size_t len = 0;
  const struct vfs_mount *m;
  for (size_t i = 0; (m = vfs_mount_at(i)) != NULL; ++i) {
    proc_append_u64(dst, cap, &len, m->device);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, m->parent);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, m->device >> 8);
    proc_append_char(dst, cap, &len, ':');
    proc_append_u64(dst, cap, &len, m->device & 0xff);
    proc_append_str(dst, cap, &len, " / ");
    proc_append_str(dst, cap, &len, m->target);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, m->options);
    proc_append_str(dst, cap, &len, " - ");
    proc_append_str(dst, cap, &len, m->fstype);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, m->source);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, m->options);
    proc_append_char(dst, cap, &len, '\n');
  }
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
  proc_append_str(dst, cap, &len, "\nvfs.page_cache_pages ");
  proc_append_u64(dst, cap, &len, vfs.page_cache_pages);
  proc_append_str(dst, cap, &len, "\nvfs.page_cache_capacity ");
  proc_append_u64(dst, cap, &len, vfs.page_cache_capacity);
  proc_append_str(dst, cap, &len, "\nvfs.page_cache_buckets ");
  proc_append_u64(dst, cap, &len, vfs.page_cache_buckets);
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
  proc_append_str(dst, cap, &len, "\next2.block_cache_flushes ");
  proc_append_u64(dst, cap, &len, ext2.block_cache_flushes);
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
  for (size_t i = 0; i < cell_domain_capacity(); ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain != NULL && domain->used) { ticks += domain->cpu_ticks; }
  }
  return ticks;
}

size_t proc_stat_text(char *dst, size_t cap) {
  size_t len = 0;
  uint32_t online = smp_online_cpu_count();
  if (online == 0) { online = 1; }
  uint64_t idle_ticks = 0;
  uint64_t busy_ticks = 0;
  for (uint32_t cpu = 0; cpu < online; ++cpu) {
    idle_ticks += cell_cpu_idle_ticks(cpu);
    busy_ticks += cell_cpu_busy_ticks(cpu);
  }
  uint64_t running = 0;
  uint64_t blocked = 0;
  for (size_t i = 0; i < cell_thread_capacity(); ++i) {
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
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0\n");
  for (uint32_t cpu = 0; cpu < online; ++cpu) {
    proc_append_str(dst, cap, &len, "cpu");
    proc_append_u64(dst, cap, &len, cpu);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, cell_cpu_busy_ticks(cpu));
    proc_append_str(dst, cap, &len, " 0 0 ");
    proc_append_u64(dst, cap, &len, cell_cpu_idle_ticks(cpu));
    proc_append_str(dst, cap, &len, " 0 0 0 0 0 0\n");
  }
  proc_append_str(dst, cap, &len, "intr 0\nctxt ");
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
