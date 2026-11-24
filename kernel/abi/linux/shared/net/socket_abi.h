#pragma once

#include <stdbool.h>
#include <stdint.h>

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

enum {
  AF_UNIX = 1,
  AF_INET = 2,
  IPPROTO_TCP = 6,
  IPPROTO_UDP = 17,
  IPPROTO_ICMP = 1,
  ENOTCONN = 107,
};

struct sockaddr_in64 {
  uint16_t sin_family;
  uint16_t sin_port;
  uint32_t sin_addr;
  uint8_t sin_zero[8];
};

bool linux_copy_sockaddr_in(uint64_t addr, uint64_t len, struct sockaddr_in64 *out);
