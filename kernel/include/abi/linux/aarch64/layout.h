#pragma once

#include <stdint.h>

struct stat64_aarch64 {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  int64_t st_atime_sec;
  int64_t st_atime_nsec;
  int64_t st_mtime_sec;
  int64_t st_mtime_nsec;
  int64_t st_ctime_sec;
  int64_t st_ctime_nsec;
  uint32_t unused_tail[2];
};

struct linux_dirent64_header {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
} __attribute__((packed));

struct timeval64 {
  int64_t tv_sec;
  int64_t tv_usec;
};

struct timespec64 {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct rusage64 {
  struct timeval64 ru_utime;
  struct timeval64 ru_stime;
  int64_t ru_maxrss;
  int64_t ru_ixrss;
  int64_t ru_idrss;
  int64_t ru_isrss;
  int64_t ru_minflt;
  int64_t ru_majflt;
  int64_t ru_nswap;
  int64_t ru_inblock;
  int64_t ru_oublock;
  int64_t ru_msgsnd;
  int64_t ru_msgrcv;
  int64_t ru_nsignals;
  int64_t ru_nvcsw;
  int64_t ru_nivcsw;
};

struct pollfd64 {
  int32_t fd;
  int16_t events;
  int16_t revents;
};

struct epoll_event64 {
  uint32_t events;
  uint64_t data;
};
