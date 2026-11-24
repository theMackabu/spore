#pragma once

#include <stdint.h>
#include <stdbool.h>

int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp);
int64_t sys_clock_getres(uint64_t clk_id, uint64_t tp);
int64_t sys_setitimer(uint64_t which, uint64_t new_addr, uint64_t old_addr);
int64_t sys_gettimeofday(uint64_t tv_addr, uint64_t tz_addr);
int64_t sys_times(uint64_t buf_addr);
