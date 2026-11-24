#pragma once

#include "abi/linux/aarch64/layout.h"

static inline uint64_t timespec_to_scheduler_ticks(const struct timespec64 *timeout) {
  uint64_t ticks =
    (uint64_t)timeout->tv_sec * 100ull + ((uint64_t)timeout->tv_nsec * 100ull + 999999999ull) / 1000000000ull;
  return ticks == 0 ? 1 : ticks;
}
