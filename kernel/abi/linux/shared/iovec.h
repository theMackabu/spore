#pragma once

#include <stdint.h>

struct iovec64 {
  uint64_t base;
  uint64_t len;
};
