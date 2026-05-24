#include "util.h"

#include <stdlib.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_getdents64
#define SYS_getdents64 61
#endif

struct linux_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : ".";
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror("ls");
    return EXIT_FAILURE;
  }
  char buf[512] = {0};
  long n = syscall(SYS_getdents64, fd, buf, sizeof(buf));
  if (n < 0) {
    perror("ls");
    close(fd);
    return EXIT_FAILURE;
  }
  int first = 1;
  for (long off = 0; off < n;) {
    struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
    if (d->d_reclen == 0 || off + d->d_reclen > n) {
      close(fd);
      eprintf("ls: bad directory entry\n");
      return EXIT_FAILURE;
    }
    printf("%s%s", first ? "" : "  ", d->d_name);
    first = 0;
    off += d->d_reclen;
  }
  printf("\n");
  close(fd);
  return EXIT_SUCCESS;
}
