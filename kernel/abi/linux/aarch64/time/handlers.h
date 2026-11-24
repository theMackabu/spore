#pragma once

#include <stdint.h>
#include <stdbool.h>

struct timespec64 syscall_wall_time(bool realtime);

struct trap_frame;

int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp);
int64_t sys_clock_getres(uint64_t clk_id, uint64_t tp);
int64_t sys_setitimer(uint64_t which, uint64_t new_addr, uint64_t old_addr);
int64_t sys_gettimeofday(uint64_t tv_addr, uint64_t tz_addr);
int64_t sys_times(uint64_t buf_addr);
int64_t sys_nanosleep(struct trap_frame *frame, uint64_t req_addr, uint64_t rem_addr);
int64_t sys_clock_nanosleep(struct trap_frame *frame, uint64_t clock_id, uint64_t flags, uint64_t req_addr,
                            uint64_t rem_addr);
