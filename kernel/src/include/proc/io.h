#pragma once

#include "arch/aarch64/regs.h"

#include <stdint.h>

int64_t cell_fd_write(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame);
int64_t cell_fd_read(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame);
