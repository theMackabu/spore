#pragma once

#include <stdint.h>
#include <stdbool.h>

struct trap_frame;

int64_t sys_sched_getaffinity(uint64_t mask, uint64_t len);
int cell_rt_sigaction(int signal, uint64_t act_addr, uint64_t old_addr, uint64_t sigset_size);
int64_t sys_sigaltstack(struct trap_frame *frame, uint64_t new_addr, uint64_t old_addr);
int64_t sys_rt_sigprocmask(struct trap_frame *frame, uint64_t how, uint64_t set, uint64_t oldset, uint64_t sigsetsize);
