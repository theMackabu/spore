#include "abi/linux/aarch64/layout.h"
#include "abi/linux/shared/layout.h"

#include <assert.h>
#include <stddef.h>

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

  assert(offsetof(struct epoll_event64, data) == 8);
  assert(sizeof(struct epoll_event64) == 16);

  assert(offsetof(struct rusage64, ru_stime) == 16);
  assert(offsetof(struct rusage64, ru_maxrss) == 32);
  assert(offsetof(struct rusage64, ru_nvcsw) == 128);
  assert(sizeof(struct rusage64) == 144);
  return 0;
}
