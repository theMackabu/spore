#pragma once

#include <stdint.h>

struct trap_frame;
void freebsd_aarch64_syscall(struct trap_frame *frame);
