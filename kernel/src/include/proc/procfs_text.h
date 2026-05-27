#pragma once

#include <stddef.h>

size_t proc_meminfo_text(char *dst, size_t cap);
size_t proc_cpuinfo_text(char *dst, size_t cap);
size_t proc_uptime_text(char *dst, size_t cap);
size_t proc_loadavg_text(char *dst, size_t cap);
size_t proc_mounts_text(char *dst, size_t cap);
size_t proc_stat_text(char *dst, size_t cap);
size_t proc_net_dev_text(char *dst, size_t cap);
size_t proc_kmsg_text(char *dst, size_t cap);
size_t proc_filesystems_text(char *dst, size_t cap);
size_t proc_partitions_text(char *dst, size_t cap);
size_t proc_devices_text(char *dst, size_t cap);
size_t proc_fsstats_text(char *dst, size_t cap);
size_t proc_fs_root_text(char *dst, size_t cap);
size_t proc_fs_boot_text(char *dst, size_t cap);
size_t proc_fs_ram0_text(char *dst, size_t cap);
size_t proc_fs_tmp_text(char *dst, size_t cap);
