#include "util.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int rc = SPORE_OK;
  for (int i = 1; i < argc; ++i) {
    int fd = open(argv[i], O_CREAT | O_WRONLY, 0666);
    if (fd < 0) {
      perror("touch");
      rc = SPORE_ERROR;
    } else {
      close(fd);
    }
  }
  return rc;
}
