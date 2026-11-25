#pragma once

#include <stdint.h>

#if defined(SPORE_NATIVE_PORT) && defined(__x86_64__)
struct linux_stat64 {
  uint64_t st_dev, st_ino, st_nlink;
  uint32_t st_mode, st_uid, st_gid, pad;
  uint64_t st_rdev;
  int64_t st_size, st_blksize, st_blocks, st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec,
    st_ctime_nsec;
  int64_t unused[3];
};
static_assert(sizeof(struct linux_stat64) == 144);
#else
struct linux_stat64 {
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

#endif

struct epoll_event64 {
  uint32_t events;
  uint64_t data;
#if defined(SPORE_NATIVE_PORT) && defined(__x86_64__)
} __attribute__((packed));
#else
};
#endif
