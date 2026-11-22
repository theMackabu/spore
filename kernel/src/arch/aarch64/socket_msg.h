#pragma once

#include <stdint.h>

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

struct iovec64 {
  uint64_t base;
  uint64_t len;
};

struct msghdr64 {
  uint64_t name;
  uint32_t namelen;
  uint32_t pad1;
  uint64_t iov;
  int32_t iovlen;
  int32_t pad2;
  uint64_t control;
  uint32_t controllen;
  uint32_t pad3;
  int32_t flags;
  int32_t pad4;
};

int aarch64_copy_socket_iov(uint64_t iov_addr, int32_t iovlen, int32_t index, struct iovec64 *out);
int64_t aarch64_unix_sendmsg(struct trap_frame *frame, uint64_t fd, const struct msghdr64 *msg, uint64_t flags);
int64_t aarch64_unix_recvmsg(struct trap_frame *frame, uint64_t fd, const struct msghdr64 *msg, uint64_t msg_addr,
                             uint64_t flags);
