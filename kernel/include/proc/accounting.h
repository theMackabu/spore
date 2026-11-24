#pragma once

#include <stdint.h>

struct domain;

struct resource_usage {
  uint64_t user_ticks, peak_pages, minor_faults, major_faults;
};

struct resource_usage cell_process_usage(const struct domain *domain);
void cell_merge_usage(struct resource_usage *total, const struct resource_usage *usage);
uint64_t cell_uptime_ticks(void);
uint64_t cell_cpu_busy_ticks(uint32_t cpu);
uint64_t cell_cpu_idle_ticks(uint32_t cpu);
void cell_loadavg_scaled(uint64_t out[3]);
uint64_t cell_boot_epoch_seconds(void);
void cell_set_boot_epoch(uint64_t epoch_sec);
uint64_t cell_realtime_seconds(void);
