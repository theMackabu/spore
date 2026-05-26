#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 3) { return usage("uds-client", "PATH MESSAGE"); }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[1]);
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    perror("connect");
    return EXIT_FAILURE;
  }
  size_t len = strlen(argv[2]);
  if (write(fd, argv[2], len) != (ssize_t)len) {
    perror("write");
    return EXIT_FAILURE;
  }
  char buf[128];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  if (n < 0) {
    perror("read");
    return EXIT_FAILURE;
  }
  buf[n] = '\0';
  printf("uds: %s\n", buf);
  close(fd);
  return EXIT_SUCCESS;
}
