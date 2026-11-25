#pragma once

#include "abi/linux/shared/net/socket_abi.h"
#include "message_layout.h"

struct trap_frame;

int linux_copy_socket_iov(uint64_t iov_addr, int32_t iovlen, int32_t index, struct iovec64 *out);
int64_t linux_unix_message(struct trap_frame *frame, int fd, uint64_t addr, const struct msghdr64 *msg, uint32_t flags,
                           bool write);
