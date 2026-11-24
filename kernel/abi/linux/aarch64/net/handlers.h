#pragma once

#include <stdint.h>

struct trap_frame;

int64_t sys_sendmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags);
int64_t sys_recvmsg(struct trap_frame *frame, uint64_t fd, uint64_t msg_addr, uint64_t flags);
