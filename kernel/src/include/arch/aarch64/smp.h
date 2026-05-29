#pragma once

#include "boot_info.h"
#include <stdbool.h>
#include <stdint.h>

void smp_init_topology(uint64_t hhdm_offset, const struct spore_cpu_entry *entries, uint32_t count);
void smp_boot_parked_secondaries(uint64_t kernel_phys_base, uint64_t kernel_virt_base);
void smp_start_scheduler_cpus(void);
uint32_t smp_present_cpu_count(void);
uint32_t smp_possible_cpu_count(void);
uint32_t smp_online_cpu_count(void);
bool smp_cpu_present(uint32_t cpu);
bool smp_cpu_online(uint32_t cpu);
uint64_t smp_cpu_mpidr(uint32_t cpu);
uint32_t smp_current_cpu(void);
void smp_set_current_cpu(uint32_t cpu);
void smp_kernel_lock(void);
void smp_kernel_unlock(void);

void *smp_current_thread_slot(uint32_t cpu);
void smp_set_current_thread_slot(uint32_t cpu, void *thread);
void smp_clear_current_thread_if(uint32_t cpu, void *thread);
bool smp_scheduler_waiting(uint32_t cpu);
void smp_set_scheduler_waiting(uint32_t cpu, bool waiting);
void smp_note_cpu_busy_tick(uint32_t cpu);
void smp_note_cpu_idle_tick(uint32_t cpu);
uint64_t smp_cpu_busy_ticks(uint32_t cpu);
uint64_t smp_cpu_idle_ticks(uint32_t cpu);
