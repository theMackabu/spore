#pragma once

#include <stdint.h>

struct trap_frame {
  alignas(16) uint64_t x[31];
  uint64_t sp_el0;
  uint64_t elr_el1;
  uint64_t spsr_el1;
  uint64_t esr_el1;
  uint64_t pad;
};

static_assert(sizeof(struct trap_frame) == 288, "AArch64 exception frame size");
