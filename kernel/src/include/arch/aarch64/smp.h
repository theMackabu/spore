#pragma once

#include <stdint.h>

enum { SPORE_MAX_CPUS = 8 };

void smp_boot_parked_secondaries(uint64_t kernel_phys_base, uint64_t kernel_virt_base);
void smp_start_scheduler_cpus(void);
uint32_t smp_parked_cpu_count(void);
uint32_t smp_online_cpu_count(void);
uint32_t smp_possible_cpu_count(void);
uint32_t smp_current_cpu(void);
void smp_set_current_cpu(uint32_t cpu);
void smp_kernel_lock(void);
void smp_kernel_unlock(void);
