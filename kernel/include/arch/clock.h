#pragma once
#include <stdint.h>

uint64_t arch_counter_value(void);
uint64_t arch_counter_frequency(void);
void syscall_set_boot_time(uint64_t epoch_sec);
