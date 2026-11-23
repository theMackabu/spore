#pragma once

#include <stddef.h>
#include <stdint.h>

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

static_assert(sizeof(struct msghdr64) == 56 && offsetof(struct msghdr64, flags) == 48, "AArch64 msghdr layout");
