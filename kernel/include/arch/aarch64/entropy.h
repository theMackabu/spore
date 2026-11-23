#pragma once

#include <stdbool.h>
#include <stdint.h>

bool aarch64_rndr_supported(void);
bool aarch64_rndr64(uint64_t *out);
