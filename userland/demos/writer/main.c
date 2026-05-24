#include <stdlib.h>

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/tmp/out";
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd < 0) {
    perror("writer");
    return EXIT_FAILURE;
  }
  const char msg[] = "hello spore\n";
  ssize_t n = write(fd, msg, sizeof(msg) - 1);
  close(fd);
  printf("writer: wrote %ld bytes\n", (long)n);
  return n == (ssize_t)(sizeof(msg) - 1) ? EXIT_SUCCESS : EXIT_FAILURE;
}
