#pragma once

#include <stdint.h>
#include <sys/syscall.h>

#ifndef SYS_getdents64
constexpr int SYS_getdents64 = 61;
#endif

struct linux_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};
