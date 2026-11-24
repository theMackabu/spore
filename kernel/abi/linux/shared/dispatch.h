#pragma once
#include <stdint.h>
struct trap_frame;
int64_t linux_dispatch(struct trap_frame *frame, uint64_t operation, const uint64_t args[6]);
