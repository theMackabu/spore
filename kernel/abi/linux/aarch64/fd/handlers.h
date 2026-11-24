#pragma once

#include <stdint.h>

int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg);
