#pragma once

#include <stdint.h>

uint64_t aarch64_current_el(void);
[[noreturn]] void aarch64_halt(void);
void aarch64_map_devices(uint64_t hhdm_offset);
[[noreturn]] void aarch64_poweroff(void);
[[noreturn]] void aarch64_reboot(void);
