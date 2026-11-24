#pragma once

#include <stdbool.h>
#include <stdint.h>

bool arch_rndr_supported(void);
bool arch_rndr64(uint64_t *out);
