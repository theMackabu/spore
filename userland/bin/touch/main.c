#include <stdlib.h>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int rc = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    int fd = openat(AT_FDCWD, argv[i], O_CREAT | O_WRONLY, 0666);
    if (fd < 0) {
      perror("touch");
      rc = EXIT_FAILURE;
    } else {
      close(fd);
    }
  }
  return rc;
}
