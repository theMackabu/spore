#include <fcntl.h>
#include <spore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
  bool all = false;
  const char *path = ".";
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
      all = true;
    } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
      return usage("ls", "[-a] [PATH]");
    } else {
      path = argv[i];
    }
  }
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
    if (!all && d->d_name[0] == '.') {
      off += d->d_reclen;
      continue;
    }
    printf("%s%s", first ? "" : "  ", d->d_name);
    first = 0;
    off += d->d_reclen;
  }
  printf("\n");
  close(fd);
  return EXIT_SUCCESS;
}
