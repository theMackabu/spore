#pragma once

#include "proc/thread.h"

struct domain;

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

enum thread_state cell_domain_state(const struct domain *domain);
size_t cell_proc_info(struct proc_info *out, size_t max);
