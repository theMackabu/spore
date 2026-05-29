#pragma once

#include <stddef.h>
#include <stdint.h>

void random_init(uint64_t seed_hint);
void random_bytes(void *dst, size_t len);
