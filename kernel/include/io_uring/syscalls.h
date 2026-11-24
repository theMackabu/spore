#pragma once
#include <stdbool.h>
#include <stdint.h>
struct trap_frame;

int64_t sys_io_uring_setup(uint32_t entries, uint64_t params);
int64_t sys_io_uring_enter(struct trap_frame *frame, int fd, uint32_t submit, uint32_t minimum, uint32_t flags, uint64_t sigmask, uint64_t sigsize, bool allow_epoll_ctl);
int64_t sys_io_uring_register(int fd, uint32_t op, uint64_t arg, uint32_t count);
