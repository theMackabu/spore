#pragma once

#include <stdint.h>

struct trap_frame;

int64_t sys_nanosleep(struct trap_frame *frame, uint64_t req_addr, uint64_t rem_addr);
int64_t sys_clock_nanosleep(struct trap_frame *frame, uint64_t clock_id, uint64_t flags, uint64_t req_addr,
                            uint64_t rem_addr);

struct timespec64 syscall_wall_time(bool realtime);
