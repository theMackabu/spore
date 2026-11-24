#pragma once

#include <stdint.h>

int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg);
int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event_addr);
