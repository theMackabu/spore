#include "util.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  char buf[256];
  for (int i = 1; i < argc; ++i) {
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) {
      perror("cat");
      return SPORE_ERROR;
    }
    for (;;) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) { break; }
      write(1, buf, (size_t)n);
    }
    close(fd);
  }
  return SPORE_OK;
}
