#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *msg = argc > 3 ? argv[3] : "hi";
  const char *port = argc > 2 ? argv[2] : "5555";
  const char *host = argc > 1 ? argv[1] : "10.0.2.2";
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM, .ai_flags = AI_NUMERICSERV};
  struct addrinfo *address;
  int rc = getaddrinfo(host, port, &hints, &address);
  if (rc != 0) {
    fprintf(stderr, "udp-send: %s: %s\n", host, gai_strerror(rc));
    return 1;
  }
  struct sockaddr_in sa = *(const struct sockaddr_in *)address->ai_addr;
  freeaddrinfo(address);
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    perror("socket");
    return 1;
  }
  ssize_t n = sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&sa, sizeof(sa));
  if (n < 0) {
    perror("sendto");
    close(fd);
    return errno == EPERM ? 1 : 2;
  }
  printf("sent %ld bytes\n", (long)n);
  close(fd);
  return 0;
}
