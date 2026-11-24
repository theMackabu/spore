#pragma once

#include <stddef.h>
#include <stdint.h>

struct trap_frame {
  uint64_t ra, t[7], a[8], s[12], gp, tp, sp, pc;
};

static_assert(offsetof(struct trap_frame, a) == 64);
static_assert(sizeof(struct trap_frame) == 256);
