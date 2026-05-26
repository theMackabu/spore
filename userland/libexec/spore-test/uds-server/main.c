#include <spore.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc != 2) { return usage("uds-server", "PATH"); }
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }
  (void)unlink(argv[1]);
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", argv[1]);
  if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 4) != 0) {
    perror("uds-server");
    return EXIT_FAILURE;
  }
  int client = accept(fd, NULL, NULL);
  if (client < 0) {
    perror("accept");
    return EXIT_FAILURE;
  }
  char buf[128];
  ssize_t n = read(client, buf, sizeof(buf));
  if (n > 0) { (void)write(client, buf, (size_t)n); }
  close(client);
  close(fd);
  return EXIT_SUCCESS;
}
