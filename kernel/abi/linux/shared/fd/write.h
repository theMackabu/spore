#pragma once

#include <stdint.h>

struct trap_frame;
int64_t sys_write(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len);
int linux_check_write_buffer(uint64_t fd, uint64_t buf, uint64_t len);
