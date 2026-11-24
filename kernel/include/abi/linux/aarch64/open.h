#pragma once

#include <stdbool.h>
#include <stdint.h>

enum {
  LINUX_O_ACCMODE = 3,
  LINUX_O_RDONLY = 0,
  LINUX_O_WRONLY = 1,
  LINUX_O_RDWR = 2,
  LINUX_O_PATH = 010000000,
};

static inline bool linux_open_readable(uint32_t flags) {
  uint32_t mode = flags & (LINUX_O_PATH | LINUX_O_ACCMODE);
  return mode == LINUX_O_RDONLY || mode == LINUX_O_RDWR;
}

static inline bool linux_open_writable(uint32_t flags) {
  uint32_t mode = flags & (LINUX_O_PATH | LINUX_O_ACCMODE);
  return mode == LINUX_O_WRONLY || mode == LINUX_O_RDWR;
}
