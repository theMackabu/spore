#include <assert.h>
#include <stddef.h>
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

int main(void) {
  assert(offsetof(struct stat64_aarch64, st_ino) == 8);
  assert(offsetof(struct stat64_aarch64, st_mode) == 16);
  assert(offsetof(struct stat64_aarch64, st_size) == 48);
  assert(offsetof(struct stat64_aarch64, st_blocks) == 64);
  assert(sizeof(struct stat64_aarch64) == 128);

  assert(offsetof(struct linux_dirent64_header, d_ino) == 0);
  assert(offsetof(struct linux_dirent64_header, d_off) == 8);
  assert(offsetof(struct linux_dirent64_header, d_reclen) == 16);
  assert(offsetof(struct linux_dirent64_header, d_type) == 18);
  assert(sizeof(struct linux_dirent64_header) == 19);
  return 0;
}
