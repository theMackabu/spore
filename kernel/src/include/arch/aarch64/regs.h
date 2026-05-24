#pragma once

#include <stdint.h>

struct trap_frame {
  uint64_t x[31];
  uint64_t sp_el0;
  uint64_t elr_el1;
  uint64_t spsr_el1;
  uint64_t esr_el1;
  uint64_t pad;
};
