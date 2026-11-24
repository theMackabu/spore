#pragma once

#include <stdint.h>

int64_t freebsd_console_syscall(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
