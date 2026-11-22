#define _GNU_SOURCE

#include "runner.h"

#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

pid_t start_udp_echo_server(void) {
  pid_t pid = fork();
  if (pid != 0) { return pid; }
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { _exit(1); }
  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(5555);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { _exit(1); }
  for (;;) {
    char buf[2048];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
    if (n > 0) { (void)sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&peer, peer_len); }
  }
}

pid_t start_http_server(void) {
  pid_t pid = fork();
  if (pid != 0) { return pid; }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { _exit(1); }
  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(8080);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) { _exit(1); }
  for (;;) {
    int client = accept(fd, NULL, NULL);
    if (client < 0) { continue; }
    char buf[1024];
    (void)read(client, buf, sizeof(buf));
    const char body[] = "hello from host http\n";
    const char header[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 21\r\nConnection: close\r\n\r\n";
    (void)write(client, header, sizeof(header) - 1);
    (void)write(client, body, sizeof(body) - 1);
    close(client);
  }
}
