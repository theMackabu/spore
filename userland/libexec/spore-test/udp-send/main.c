#include <spore.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static uint16_t be16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

int main(int argc, char **argv) {
  const char *msg = argc > 3 ? argv[3] : "hi";
  int port = argc > 2 ? atoi(argv[2]) : 5555;
  const char *host = argc > 1 ? argv[1] : "10.0.2.2";
  uint32_t ip = 0;
  if (!resolve_ipv4(host, &ip)) {
    fprintf(stderr, "udp-send: bad address: %s\n", host);
    return 1;
  }
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = be16((uint16_t)port);
  sa.sin_addr.s_addr = ip;
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
