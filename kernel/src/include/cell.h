#pragma once

#include <stdbool.h>
#include <stdint.h>

struct trap_frame;
struct user_address_space;
struct vma_list;

void cell_system_init(uint64_t hhdm_offset);
bool cell_create_init(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp);
uint64_t cell_uptime_ticks(void);
uint64_t cell_idle_ticks(void);
uint64_t cell_cpu_busy_ticks(uint32_t cpu);
uint64_t cell_cpu_idle_ticks(uint32_t cpu);
void cell_loadavg_scaled(uint64_t out[3]);
uint64_t cell_boot_epoch_seconds(void);
void cell_timer_tick(struct trap_frame *frame, bool from_lower_el);
void cell_set_boot_epoch(uint64_t epoch_sec);
uint64_t cell_realtime_seconds(void);
