#pragma once

#include "message_layout.h"
#include <stdbool.h>

struct trap_frame;

enum {
  SOL_SOCKET = 1,
  EBADF = 9,
  EFAULT = 14,
  EINVAL = 22,
  EPIPE = 32,
  SIGPIPE = 13,
  FD_CLOEXEC = 1,
  MSG_DONTWAIT = 0x40,
  MSG_NOSIGNAL = 0x4000,
};

int aarch64_copy_socket_iov(uint64_t iov_addr, int32_t iovlen, int32_t index, struct iovec64 *out);
int64_t aarch64_unix_message(struct trap_frame *frame, int fd, uint64_t addr, const struct msghdr64 *msg,
                             uint32_t flags, bool write);
