#include <stdlib.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "/etc/motd";
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    printf("peeker: open(%s): Operation not permitted\n", path);
    return errno == EPERM ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  close(fd);
  puts("peeker: unexpected open success");
  return EXIT_FAILURE;
}
