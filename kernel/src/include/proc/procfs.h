#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;
struct open_file;

struct proc_info {
  uint32_t pid;
  uint32_t tid;
  uint32_t ppid;
  uint32_t state;
  uint32_t wait_reason;
  uint32_t _pad;
  uint64_t resident_pages;
  uint64_t virtual_pages;
  uint64_t minor_faults;
  uint64_t major_faults;
  uint64_t cpu_ticks;
  uint64_t start_ticks;
  uint64_t remaining_ticks;
  uint64_t max_ticks;
  char name[32];
  char exec_path[128];
  char argv0[64];
  char cmdline[160];
  char cwd[64];
};

int64_t cell_procfs_read_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len);
bool cell_proc_info_for_pid(int pid, struct proc_info *out);
size_t cell_proc_info(struct proc_info *out, size_t max);
