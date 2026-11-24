#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef SPORE_USERLAND
extern bool app_mode;
#else
constexpr bool app_mode = true;
#endif

enum app_result { APP_EXIT = 'E', APP_SIGNAL = 'S', APP_BOOT_ERROR = 'B' };

uint64_t app_payload_size(void);
void app_track_entry(uint64_t pc);
void app_instruction_fetched(uint64_t pc);
[[noreturn]] void app_finish(enum app_result kind, unsigned value);
