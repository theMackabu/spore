#pragma once

#include <stdbool.h>
#include <stdint.h>

struct domain;
struct thread;
struct trap_frame;

void cell_futex_cleanup_robust_list(struct thread *thread);
int cell_futex_wake_domain(struct domain *domain, uint64_t uaddr, uint32_t count);
int cell_set_robust_list_current(uint64_t robust_list);
int cell_futex_wait_current(uint64_t uaddr, uint32_t expected, bool has_deadline, uint64_t deadline_tick,
                            struct trap_frame *frame);
int cell_futex_wake_current(uint64_t uaddr, uint32_t count);
