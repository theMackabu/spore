#include "integration_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define SYS_SENDTO 206
#define SYS_ACCEPT4 242

static uint16_t integration_be16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

uint32_t integration_ip4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a | (b << 8) | (c << 16) | (d << 24);
}

void integration_fill_udp_addr(struct sockaddr_in *sa, uint32_t ip, uint16_t port) {
  memset(sa, 0, sizeof(*sa));
  sa->sin_family = AF_INET;
  sa->sin_port = integration_be16(port);
  sa->sin_addr.s_addr = ip;
}

int integration_udp_demo(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] v3e udp socket echo: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(10, 0, 2, 2), 5555);
  const char msg[] = "udp-hi";
  ssize_t sent = sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  char buf[32] = {0};
  ssize_t got = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
  close(fd);
  int ok = sent == (ssize_t)(sizeof(msg) - 1) && got == (ssize_t)(sizeof(msg) - 1) && strcmp(buf, msg) == 0;
  printf("[spore] v3e udp socket echo: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok;
}

static int udp_bind_conflict_regression(void) {
  int a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (a < 0 || b < 0) {
    if (a >= 0) { close(a); }
    if (b >= 0) { close(b); }
    printf("[spore] udp bind conflict: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(0, 0, 0, 0), 45678);
  int first = bind(a, (struct sockaddr *)&sa, sizeof(sa));
  errno = 0;
  int second = bind(b, (struct sockaddr *)&sa, sizeof(sa));
  int ok = first == 0 && second < 0 && errno == EADDRINUSE;
  close(a);
  close(b);
  printf("[spore] udp bind conflict: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int udp_receive_queue_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp receive queue: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(10, 0, 2, 2), 5555);
  const char first[] = "udp-q1";
  const char second[] = "udp-q2";
  ssize_t sent_first = sendto(fd, first, sizeof(first) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  ssize_t sent_second = sendto(fd, second, sizeof(second) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  char got_first[16] = {0};
  char got_second[16] = {0};
  ssize_t read_first = recvfrom(fd, got_first, sizeof(got_first) - 1, 0, NULL, NULL);
  ssize_t read_second = recvfrom(fd, got_second, sizeof(got_second) - 1, 0, NULL, NULL);
  close(fd);
  int ok = sent_first == (ssize_t)(sizeof(first) - 1) && sent_second == (ssize_t)(sizeof(second) - 1) &&
           read_first == (ssize_t)(sizeof(first) - 1) && read_second == (ssize_t)(sizeof(second) - 1) &&
           strcmp(got_first, first) == 0 && strcmp(got_second, second) == 0;
  printf("[spore] udp receive queue: %s (%s,%s)\n", ok ? "PASS" : "FAIL", got_first, got_second);
  return ok;
}

static int udp_msg_iov_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp msg iov: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(10, 0, 2, 2), 5555);
  const char a[] = "iov-";
  const char b[] = "send-";
  const char c[] = "recv";
  struct iovec out_iov[3] = {
    {.iov_base = (void *)a, .iov_len = sizeof(a) - 1},
    {.iov_base = (void *)b, .iov_len = sizeof(b) - 1},
    {.iov_base = (void *)c, .iov_len = sizeof(c) - 1},
  };
  struct msghdr out = {
    .msg_name = &sa,
    .msg_namelen = sizeof(sa),
    .msg_iov = out_iov,
    .msg_iovlen = 3,
  };
  ssize_t sent = sendmsg(fd, &out, 0);

  char first[5] = {0};
  char second[10] = {0};
  struct iovec in_iov[2] = {
    {.iov_base = first, .iov_len = sizeof(first) - 1},
    {.iov_base = second, .iov_len = sizeof(second) - 1},
  };
  struct sockaddr_in from;
  socklen_t from_len = sizeof(from);
  struct msghdr in = {
    .msg_name = &from,
    .msg_namelen = from_len,
    .msg_iov = in_iov,
    .msg_iovlen = 2,
  };
  ssize_t got = recvmsg(fd, &in, 0);
  close(fd);
  int ok = sent == 13 && got == 13 && strcmp(first, "iov-") == 0 && strcmp(second, "send-recv") == 0 &&
           in.msg_namelen == sizeof(from);
  printf("[spore] udp msg iov: %s (%s%s)\n", ok ? "PASS" : "FAIL", first, second);
  return ok;
}

static int udp_recvmsg_trunc_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp recvmsg trunc: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(10, 0, 2, 2), 5555);
  const char payload[] = "truncate-this";

  char small[5] = {0};
  struct iovec iov = {.iov_base = small, .iov_len = 4};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
  ssize_t sent = sendto(fd, payload, sizeof(payload) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  ssize_t got = recvmsg(fd, &msg, 0);
  int ok = sent == (ssize_t)(sizeof(payload) - 1) && got == 4 && strcmp(small, "trun") == 0 &&
           (msg.msg_flags & MSG_TRUNC) != 0;

  memset(small, 0, sizeof(small));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  sent = sendto(fd, payload, sizeof(payload) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  got = recvmsg(fd, &msg, MSG_TRUNC);
  ok = ok && sent == (ssize_t)(sizeof(payload) - 1) && got == (ssize_t)(sizeof(payload) - 1) &&
       strcmp(small, "trun") == 0 && (msg.msg_flags & MSG_TRUNC) != 0;

  close(fd);
  printf("[spore] udp recvmsg trunc: %s got=%zd flags=0x%x\n", ok ? "PASS" : "FAIL", got, msg.msg_flags);
  return ok;
}

static int socket_msg_peek_regression(void) {
  int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp < 0) {
    printf("[spore] socket MSG_PEEK: FAIL udp socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(10, 0, 2, 2), 5555);
  const char first[] = "peek-one";
  const char second[] = "peek-two";
  char peeked[16] = {0};
  char got_first[16] = {0};
  char got_second[16] = {0};
  ssize_t sent_first = sendto(udp, first, sizeof(first) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  ssize_t sent_second = sendto(udp, second, sizeof(second) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  ssize_t peek = recvfrom(udp, peeked, sizeof(peeked) - 1, MSG_PEEK, NULL, NULL);
  ssize_t read_first = recvfrom(udp, got_first, sizeof(got_first) - 1, 0, NULL, NULL);
  ssize_t read_second = recvfrom(udp, got_second, sizeof(got_second) - 1, 0, NULL, NULL);
  int ok = sent_first == (ssize_t)(sizeof(first) - 1) && sent_second == (ssize_t)(sizeof(second) - 1) &&
           peek == (ssize_t)(sizeof(first) - 1) && read_first == (ssize_t)(sizeof(first) - 1) &&
           read_second == (ssize_t)(sizeof(second) - 1) && strcmp(peeked, first) == 0 &&
           strcmp(got_first, first) == 0 && strcmp(got_second, second) == 0;
  close(udp);

  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] socket MSG_PEEK: FAIL tcp socket\n");
    return 0;
  }
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), 45685);
  ok = ok && bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
       connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  const char stream[] = "peek-stream";
  char stream_peek[8] = {0};
  char stream_read[16] = {0};
  if (ok) {
    ok = send(accepted, stream, sizeof(stream) - 1, 0) == (ssize_t)(sizeof(stream) - 1) &&
         recv(client, stream_peek, 4, MSG_PEEK) == 4 && strcmp(stream_peek, "peek") == 0 &&
         recv(client, stream_read, sizeof(stream_read) - 1, 0) == (ssize_t)(sizeof(stream) - 1) &&
         strcmp(stream_read, stream) == 0;
  }

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] socket MSG_PEEK: %s udp=%s/%s/%s tcp=%s/%s\n", ok ? "PASS" : "FAIL", peeked, got_first, got_second,
         stream_peek, stream_read);
  return ok;
}

static int socket_msg_waitall_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] socket MSG_WAITALL: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), 45686);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  char buf[16] = {0};
  if (ok) {
    ok = send(accepted, "wait", 4, 0) == 4;
    pid_t child = fork();
    if (child == 0) {
      usleep(20000);
      (void)send(accepted, "-all", 4, 0);
      _exit(0);
    }
    ssize_t got = recv(client, buf, 8, MSG_WAITALL);
    int status = 0;
    if (child > 0) { (void)waitpid(child, &status, 0); }
    ok = ok && child > 0 && got == 8 && memcmp(buf, "wait-all", 8) == 0;
  }

  char iov_a[5] = {0};
  char iov_b[5] = {0};
  if (ok) {
    ok = send(accepted, "iov-", 4, 0) == 4;
    pid_t child = fork();
    if (child == 0) {
      usleep(20000);
      (void)send(accepted, "done", 4, 0);
      _exit(0);
    }
    struct iovec iov[2] = {
      {.iov_base = iov_a, .iov_len = 4},
      {.iov_base = iov_b, .iov_len = 4},
    };
    struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
    ssize_t got = recvmsg(client, &msg, MSG_WAITALL);
    int status = 0;
    if (child > 0) { (void)waitpid(child, &status, 0); }
    ok = ok && child > 0 && got == 8 && memcmp(iov_a, "iov-", 4) == 0 && memcmp(iov_b, "done", 4) == 0;
  }

  char partial[8] = {0};
  if (ok) {
    ok = send(accepted, "mini", 4, 0) == 4;
    errno = 0;
    ssize_t got = recv(client, partial, sizeof(partial), MSG_WAITALL | MSG_DONTWAIT);
    ok = ok && got == 4 && memcmp(partial, "mini", 4) == 0;
  }

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] socket MSG_WAITALL: %s recv=%.*s recvmsg=%.*s%.*s dontwait=%.*s\n", ok ? "PASS" : "FAIL", 8, buf, 4,
         iov_a, 4, iov_b, 4, partial);
  return ok;
}

static int getsockname_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] getsockname: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in bind_sa;
  integration_fill_udp_addr(&bind_sa, integration_ip4(0, 0, 0, 0), 45679);
  struct sockaddr_in got_sa;
  socklen_t got_len = sizeof(got_sa);
  int ok = bind(fd, (struct sockaddr *)&bind_sa, sizeof(bind_sa)) == 0 &&
           getsockname(fd, (struct sockaddr *)&got_sa, &got_len) == 0 && got_sa.sin_port == bind_sa.sin_port &&
           got_len == sizeof(got_sa);
  close(fd);
  printf("[spore] getsockname: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int socket_options_regression(void) {
  int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (udp < 0 || tcp < 0) {
    if (udp >= 0) { close(udp); }
    if (tcp >= 0) { close(tcp); }
    printf("[spore] socket options: FAIL socket\n");
    return 0;
  }
  int one = 1;
  int got = 0;
  socklen_t got_len = sizeof(got);
  int sndbuf = 65536;
  int small_rcvbuf = 4096;
  int huge_buf = 1048576;
  int tcp_sndbuf = 0;
  int tcp_rcvbuf = 0;
  int tos = 0xb8;
  int ttl = 37;
  int user_timeout = 1234;
  int rcvlowat = 7;
  int keepidle = 1;
  int keepintvl = 1;
  int keepcnt = 2;
  int mtu_discover = 1;
  int bind_no_port = 1;
  struct linger linger = {.l_onoff = 1, .l_linger = 0};
  struct linger got_linger = {0};
  socklen_t linger_len = sizeof(got_linger);
  struct timeval tv = {.tv_sec = 1, .tv_usec = 250000};
  int ok =
    setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
    setsockopt(udp, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) == 0 &&
    setsockopt(udp, SOL_SOCKET, SO_DONTROUTE, &one, sizeof(one)) == 0 &&
    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
    setsockopt(udp, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0 &&
    setsockopt(udp, SOL_SOCKET, SO_RCVBUF, &small_rcvbuf, sizeof(small_rcvbuf)) == 0 &&
    getsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &got, &got_len) == 0 && got == 1 && got_len == sizeof(got) &&
    getsockopt(udp, SOL_SOCKET, SO_REUSEPORT, &got, &got_len) == 0 && got == 1 && got_len == sizeof(got) &&
    getsockopt(udp, SOL_SOCKET, SO_DONTROUTE, &got, &got_len) == 0 && got == 1 && got_len == sizeof(got) &&
    setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0 &&
    setsockopt(tcp, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) == 0 &&
    setsockopt(tcp, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)) == 0 &&
    setsockopt(tcp, SOL_SOCKET, SO_RCVLOWAT, &rcvlowat, sizeof(rcvlowat)) == 0 &&
    setsockopt(tcp, SOL_SOCKET, SO_SNDBUF, &huge_buf, sizeof(huge_buf)) == 0 &&
    setsockopt(tcp, SOL_SOCKET, SO_RCVBUF, &huge_buf, sizeof(huge_buf)) == 0 &&
    setsockopt(tcp, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) == 0 &&
    setsockopt(tcp, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) == 0 &&
    setsockopt(tcp, IPPROTO_IP, IP_MTU_DISCOVER, &mtu_discover, sizeof(mtu_discover)) == 0 &&
    setsockopt(tcp, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &bind_no_port, sizeof(bind_no_port)) == 0 &&
    setsockopt(tcp, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) == 0 &&
    setsockopt(tcp, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) == 0 &&
    setsockopt(tcp, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) == 0 &&
    setsockopt(tcp, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout, sizeof(user_timeout)) == 0 &&
    setsockopt(tcp, IPPROTO_TCP, TCP_FASTOPEN, &one, sizeof(one)) == 0 &&
    setsockopt(tcp, IPPROTO_TCP, TCP_FASTOPEN_CONNECT, &one, sizeof(one)) == 0 &&
    getsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, &got, &got_len) == 0 && got == 1 && got_len == sizeof(got) &&
    getsockopt(tcp, IPPROTO_IP, IP_TOS, &got, &got_len) == 0 && got == tos && got_len == sizeof(got) &&
    getsockopt(tcp, IPPROTO_IP, IP_TTL, &got, &got_len) == 0 && got == ttl && got_len == sizeof(got) &&
    getsockopt(tcp, IPPROTO_IP, IP_MTU_DISCOVER, &got, &got_len) == 0 && got == mtu_discover &&
    got_len == sizeof(got) && getsockopt(tcp, IPPROTO_IP, IP_BIND_ADDRESS_NO_PORT, &got, &got_len) == 0 &&
    got == bind_no_port && got_len == sizeof(got) && getsockopt(tcp, IPPROTO_TCP, TCP_KEEPIDLE, &got, &got_len) == 0 &&
    got == keepidle && got_len == sizeof(got) && getsockopt(tcp, IPPROTO_TCP, TCP_KEEPINTVL, &got, &got_len) == 0 &&
    got == keepintvl && got_len == sizeof(got) && getsockopt(tcp, IPPROTO_TCP, TCP_KEEPCNT, &got, &got_len) == 0 &&
    got == keepcnt && got_len == sizeof(got) && getsockopt(tcp, IPPROTO_TCP, TCP_USER_TIMEOUT, &got, &got_len) == 0 &&
    got == user_timeout && got_len == sizeof(got) && getsockopt(tcp, SOL_SOCKET, SO_KEEPALIVE, &got, &got_len) == 0 &&
    got == 1 && got_len == sizeof(got) && getsockopt(tcp, SOL_SOCKET, SO_RCVLOWAT, &got, &got_len) == 0 &&
    got == rcvlowat && got_len == sizeof(got) && getsockopt(tcp, SOL_SOCKET, SO_SNDLOWAT, &got, &got_len) == 0 &&
    got == 1 && got_len == sizeof(got) && getsockopt(tcp, SOL_SOCKET, SO_ACCEPTCONN, &got, &got_len) == 0 && got == 0 &&
    got_len == sizeof(got) && getsockopt(tcp, SOL_SOCKET, SO_DOMAIN, &got, &got_len) == 0 && got == AF_INET &&
    got_len == sizeof(got) && getsockopt(tcp, SOL_SOCKET, SO_PROTOCOL, &got, &got_len) == 0 && got == IPPROTO_TCP &&
    got_len == sizeof(got) && getsockopt(tcp, SOL_SOCKET, SO_LINGER, &got_linger, &linger_len) == 0 &&
    linger_len == sizeof(got_linger) && got_linger.l_onoff == 1 && got_linger.l_linger == 0 &&
    getsockopt(udp, SOL_SOCKET, SO_SNDBUF, &got, &got_len) == 0 && got == sndbuf && got_len == sizeof(got) &&
    getsockopt(udp, SOL_SOCKET, SO_RCVBUF, &got, &got_len) == 0 && got == small_rcvbuf && got_len == sizeof(got) &&
    getsockopt(tcp, SOL_SOCKET, SO_SNDBUF, &tcp_sndbuf, &got_len) == 0 && tcp_sndbuf > 0 && tcp_sndbuf < huge_buf &&
    got_len == sizeof(tcp_sndbuf) && getsockopt(tcp, SOL_SOCKET, SO_RCVBUF, &tcp_rcvbuf, &got_len) == 0 &&
    tcp_rcvbuf > 0 && tcp_rcvbuf < huge_buf && got_len == sizeof(tcp_rcvbuf);
  close(udp);
  close(tcp);
  printf("[spore] socket options: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int tcp_rcvlowat_poll_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] tcp SO_RCVLOWAT poll: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), 45692);
  int accepting = 0;
  socklen_t accepting_len = sizeof(accepting);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           getsockopt(server, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_len) == 0 && accepting == 1 &&
           accepting_len == sizeof(accepting) && connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  int lowat = 5;
  struct pollfd pfd = {.fd = client, .events = POLLIN};
  int below = -1;
  int ready = -1;
  char buf[8] = {0};
  if (ok) {
    ok = setsockopt(client, SOL_SOCKET, SO_RCVLOWAT, &lowat, sizeof(lowat)) == 0 && send(accepted, "abc", 3, 0) == 3;
    below = poll(&pfd, 1, 0);
    ok = ok && below == 0 && send(accepted, "de", 2, 0) == 2;
    pfd.revents = 0;
    ready = poll(&pfd, 1, 0);
    ok =
      ok && ready == 1 && (pfd.revents & POLLIN) != 0 && recv(client, buf, 5, 0) == 5 && memcmp(buf, "abcde", 5) == 0;
  }

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] tcp SO_RCVLOWAT poll: %s below=%d ready=%d data=%.*s\n", ok ? "PASS" : "FAIL", below, ready, 5, buf);
  return ok;
}

static int tcp_abortive_linger_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] tcp SO_LINGER reset: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), 45693);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  struct linger linger = {.l_onoff = 1, .l_linger = 0};
  struct pollfd pfd = {.fd = client, .events = POLLIN};
  char ch = 0;
  int poll_rc = -1;
  int recv_errno = 0;
  if (ok) {
    ok = setsockopt(accepted, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)) == 0;
    close(accepted);
    accepted = -1;
    poll_rc = poll(&pfd, 1, 0);
    errno = 0;
    ssize_t n = recv(client, &ch, 1, 0);
    recv_errno = errno;
    ok = ok && poll_rc == 1 && (pfd.revents & POLLERR) != 0 && n < 0 && recv_errno == ECONNRESET;
  }

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] tcp SO_LINGER reset: %s poll=%d revents=0x%x errno=%d\n", ok ? "PASS" : "FAIL", poll_rc, pfd.revents,
         recv_errno);
  return ok;
}

static int udp_broadcast_option_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp broadcast option: FAIL socket\n");
    return 0;
  }

  char payload[] = "broadcast";
  struct sockaddr_in dst;
  integration_fill_udp_addr(&dst, integration_ip4(255, 255, 255, 255), 45687);

  errno = 0;
  ssize_t denied = sendto(fd, payload, sizeof(payload), 0, (struct sockaddr *)&dst, sizeof(dst));
  int denied_errno = errno;

  int one = 1;
  errno = 0;
  int set_ok = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  ssize_t sent = sendto(fd, payload, sizeof(payload), 0, (struct sockaddr *)&dst, sizeof(dst));
  int send_errno = errno;

  close(fd);

  int ok = denied < 0 && denied_errno == EACCES && set_ok == 0 && sent == (ssize_t)sizeof(payload);
  printf("[spore] udp broadcast option: %s denied=%d sent=%zd errno=%d\n", ok ? "PASS" : "FAIL", denied_errno, sent,
         send_errno);
  return ok;
}

static int udp_loopback_refused_regression(void) {
  char payload[] = "x";
  struct sockaddr_in closed;
  integration_fill_udp_addr(&closed, integration_ip4(127, 0, 0, 1), 45992);

  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp loopback refused: FAIL socket\n");
    return 0;
  }
  int ok = connect(fd, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
           send(fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);

  struct pollfd pfd = {.fd = fd, .events = 0};
  int poll_rc = poll(&pfd, 1, 0);
  int so_error = 0;
  socklen_t so_error_len = sizeof(so_error);
  int get_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
  int after_error = -1;
  so_error_len = sizeof(after_error);
  int get_after_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &after_error, &so_error_len);
  close(fd);

  int recv_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  ok = ok && recv_fd >= 0 && connect(recv_fd, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
       send(recv_fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);
  char byte = 0;
  errno = 0;
  ssize_t recv_rc = recv(recv_fd, &byte, sizeof(byte), MSG_DONTWAIT);
  int recv_errno = errno;
  if (recv_fd >= 0) { close(recv_fd); }

  int recvmsg_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  ok = ok && recvmsg_fd >= 0 && connect(recvmsg_fd, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
       send(recvmsg_fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);
  struct iovec iov = {.iov_base = &byte, .iov_len = sizeof(byte)};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
  errno = 0;
  ssize_t recvmsg_rc = recvmsg(recvmsg_fd, &msg, MSG_DONTWAIT);
  int recvmsg_errno = errno;
  if (recvmsg_fd >= 0) { close(recvmsg_fd); }

  int epoll_fd = epoll_create1(0);
  int epoll_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int epoll_rc = -1;
  uint32_t epoll_events = 0;
  if (epoll_fd >= 0 && epoll_sock >= 0 && connect(epoll_sock, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
      send(epoll_sock, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload)) {
    struct epoll_event ev = {.events = 0, .data.u32 = 0x55};
    struct epoll_event out = {0};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, epoll_sock, &ev) == 0) {
      epoll_rc = epoll_wait(epoll_fd, &out, 1, 0);
      epoll_events = out.events;
    }
  }
  if (epoll_sock >= 0) { close(epoll_sock); }
  if (epoll_fd >= 0) { close(epoll_fd); }

  int unconnected = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int unconnected_error = -1;
  socklen_t unconnected_len = sizeof(unconnected_error);
  ssize_t unconnected_send = -1;
  int unconnected_get = -1;
  if (unconnected >= 0) {
    unconnected_send = sendto(unconnected, payload, sizeof(payload), 0, (struct sockaddr *)&closed, sizeof(closed));
    unconnected_get = getsockopt(unconnected, SOL_SOCKET, SO_ERROR, &unconnected_error, &unconnected_len);
    close(unconnected);
  }

  ok = ok && poll_rc == 1 && (pfd.revents & POLLERR) != 0 && get_rc == 0 && so_error == ECONNREFUSED &&
       get_after_rc == 0 && after_error == 0 && recv_rc < 0 && recv_errno == ECONNREFUSED && recvmsg_rc < 0 &&
       recvmsg_errno == ECONNREFUSED && epoll_rc == 1 && (epoll_events & EPOLLERR) != 0 &&
       unconnected_send == (ssize_t)sizeof(payload) && unconnected_get == 0 && unconnected_error == 0;
  printf("[spore] udp loopback refused: %s poll=0x%x epoll=0x%x so_error=%d recv=%d recvmsg=%d unconnected=%d\n",
         ok ? "PASS" : "FAIL", pfd.revents, epoll_events, so_error, recv_errno, recvmsg_errno, unconnected_error);
  return ok;
}

static int udp_disconnect_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp disconnect: FAIL socket\n");
    return 0;
  }

  char payload[] = "x";
  struct sockaddr_in peer;
  integration_fill_udp_addr(&peer, integration_ip4(127, 0, 0, 1), 45993);
  int ok = connect(fd, (struct sockaddr *)&peer, sizeof(peer)) == 0 &&
           send(fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);

  struct sockaddr unspec;
  memset(&unspec, 0, sizeof(unspec));
  ok = ok && connect(fd, &unspec, sizeof(unspec)) == 0;

  int so_error = -1;
  socklen_t so_error_len = sizeof(so_error);
  int get_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);

  struct sockaddr_in got_peer;
  socklen_t got_peer_len = sizeof(got_peer);
  errno = 0;
  int peer_rc = getpeername(fd, (struct sockaddr *)&got_peer, &got_peer_len);
  int peer_errno = errno;

  errno = 0;
  ssize_t send_rc = send(fd, payload, sizeof(payload), 0);
  int send_errno = errno;

  ssize_t sendto_rc = sendto(fd, payload, sizeof(payload), 0, (struct sockaddr *)&peer, sizeof(peer));
  close(fd);

  ok = ok && get_rc == 0 && so_error == 0 && peer_rc < 0 && peer_errno == ENOTCONN && send_rc < 0 &&
       send_errno == EINVAL && sendto_rc == (ssize_t)sizeof(payload);
  printf("[spore] udp disconnect: %s so_error=%d peer=%d send=%d sendto=%zd\n", ok ? "PASS" : "FAIL", so_error,
         peer_errno, send_errno, sendto_rc);
  return ok;
}

static int udp_external_refused_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp external refused: FAIL socket\n");
    return 0;
  }

  char payload[] = "x";
  struct sockaddr_in closed;
  integration_fill_udp_addr(&closed, integration_ip4(10, 0, 2, 2), 45994);
  int ok = connect(fd, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
           send(fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);

  struct pollfd pfd = {.fd = fd, .events = 0};
  int poll_rc = poll(&pfd, 1, 250);

  int so_error = 0;
  socklen_t so_error_len = sizeof(so_error);
  int get_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
  close(fd);

  if (poll_rc == 0 && get_rc == 0 && so_error == 0) {
    printf("[spore] udp external refused: SKIP no host ICMP error\n");
    return 1;
  }

  ok = ok && poll_rc == 1 && (pfd.revents & POLLERR) != 0 && get_rc == 0 && so_error == ECONNREFUSED;
  printf("[spore] udp external refused: %s poll=0x%x so_error=%d\n", ok ? "PASS" : "FAIL", pfd.revents, so_error);
  return ok;
}

static int socket_timeout_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] socket timeout: FAIL socket\n");
    return 0;
  }
  struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
  struct timeval got_tv = {0};
  socklen_t got_len = sizeof(got_tv);
  char buf[8];
  errno = 0;
  int ok = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &got_tv, &got_len) == 0 && got_len == sizeof(got_tv) &&
           got_tv.tv_sec == 0 && got_tv.tv_usec == 20000 && recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK);
  close(fd);
  printf("[spore] socket timeout: %s errno=%d\n", ok ? "PASS" : "FAIL", errno);
  return ok;
}

static int socket_msg_dontwait_regression(void) {
  int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp < 0) {
    printf("[spore] socket MSG_DONTWAIT: FAIL udp socket\n");
    return 0;
  }

  char byte = 0;
  errno = 0;
  uint64_t start = integration_usec_now();
  ssize_t udp_recv = recvfrom(udp, &byte, sizeof(byte), MSG_DONTWAIT, NULL, NULL);
  uint64_t udp_elapsed = integration_usec_now() - start;
  int ok = udp_recv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && udp_elapsed < 5000;

  struct iovec iov = {.iov_base = &byte, .iov_len = sizeof(byte)};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
  errno = 0;
  start = integration_usec_now();
  ssize_t udp_msg = recvmsg(udp, &msg, MSG_DONTWAIT);
  uint64_t udp_msg_elapsed = integration_usec_now() - start;
  ok = ok && udp_msg < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && udp_msg_elapsed < 5000;
  close(udp);

  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] socket MSG_DONTWAIT: FAIL tcp socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), 45684);
  ok = ok && bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
       connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  errno = 0;
  start = integration_usec_now();
  ssize_t tcp_recv = recv(client, &byte, sizeof(byte), MSG_DONTWAIT);
  uint64_t tcp_elapsed = integration_usec_now() - start;
  ok = ok && tcp_recv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && tcp_elapsed < 5000;

  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  errno = 0;
  start = integration_usec_now();
  ssize_t tcp_msg = recvmsg(client, &msg, MSG_DONTWAIT);
  uint64_t tcp_msg_elapsed = integration_usec_now() - start;
  ok = ok && tcp_msg < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && tcp_msg_elapsed < 5000;

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] socket MSG_DONTWAIT: %s udp=%luus/%luus tcp=%luus/%luus\n", ok ? "PASS" : "FAIL",
         (unsigned long)udp_elapsed, (unsigned long)udp_msg_elapsed, (unsigned long)tcp_elapsed,
         (unsigned long)tcp_msg_elapsed);
  return ok;
}

static int socket_shutdown_regression(void) {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    printf("[spore] socket shutdown: FAIL socket\n");
    return 0;
  }
  errno = 0;
  int rc = shutdown(fd, SHUT_WR);
  int ok = rc < 0 && errno == ENOTCONN;
  close(fd);
  printf("[spore] socket shutdown: %s errno=%d\n", ok ? "PASS" : "FAIL", errno);
  return ok;
}

static int open_tcp_loopback_pair(uint16_t port, int *client_out, int *accepted_out) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    return 0;
  }
  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), port);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  close(server);
  if (!ok || accepted < 0) {
    close(client);
    if (accepted >= 0) { close(accepted); }
    return 0;
  }
  *client_out = client;
  *accepted_out = accepted;
  return 1;
}

static void tcp_sigpipe_child(int mode) {
  int client = -1;
  int accepted = -1;
  if (!open_tcp_loopback_pair((uint16_t)(45685 + mode), &client, &accepted)) { integration_exit_group(70); }
  if (shutdown(client, SHUT_WR) != 0) { integration_exit_group(71); }
  if (mode == 0 && signal(SIGPIPE, SIG_DFL) == SIG_ERR) { integration_exit_group(73); }
  if (mode == 1 && signal(SIGPIPE, SIG_IGN) == SIG_ERR) { integration_exit_group(74); }

  char byte = 'x';
  long rc = integration_raw_syscall6(SYS_SENDTO, client, (long)&byte, 1, mode == 2 ? MSG_NOSIGNAL : 0, 0, 0);
  close(client);
  close(accepted);
  integration_exit_group(rc == -EPIPE ? 0 : 72);
}

static int tcp_sigpipe_regression(void) {
  pid_t default_pid = fork();
  if (default_pid == 0) { tcp_sigpipe_child(0); }
  int default_status = 0;
  waitpid(default_pid, &default_status, 0);

  pid_t ignored_pid = fork();
  if (ignored_pid == 0) { tcp_sigpipe_child(1); }
  int ignored_status = 0;
  waitpid(ignored_pid, &ignored_status, 0);

  pid_t nosignal_pid = fork();
  if (nosignal_pid == 0) { tcp_sigpipe_child(2); }
  int nosignal_status = 0;
  waitpid(nosignal_pid, &nosignal_status, 0);

  int default_ok = WIFSIGNALED(default_status) && WTERMSIG(default_status) == SIGPIPE;
  int ignored_ok = WIFEXITED(ignored_status) && WEXITSTATUS(ignored_status) == 0;
  int nosignal_ok = WIFEXITED(nosignal_status) && WEXITSTATUS(nosignal_status) == 0;
  int ok = default_ok && ignored_ok && nosignal_ok;
  printf("[spore] tcp SIGPIPE: %s default-sig=%d ignored=%d nosignal=%d\n", ok ? "PASS" : "FAIL",
         WIFSIGNALED(default_status) ? WTERMSIG(default_status) : 0, WEXITSTATUS(ignored_status),
         WEXITSTATUS(nosignal_status));
  return ok;
}

static int tcp_loopback_accept_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] tcp loopback accept: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), 45681);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 4) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;

  struct pollfd pfd = {.fd = server, .events = POLLIN};
  ok = ok && poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0;

  struct sockaddr_in accepted_peer;
  memset(&accepted_peer, 0, sizeof(accepted_peer));
  socklen_t accepted_peer_len = sizeof(accepted_peer);
  int accepted = ok ? (int)integration_raw_syscall5(SYS_ACCEPT4, server, (long)&accepted_peer, (long)&accepted_peer_len,
                                                    SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
                    : -1;
  ok = ok && accepted >= 0;
  if (ok) {
    int status_flags = fcntl(accepted, F_GETFL, 0);
    int fd_flags = fcntl(accepted, F_GETFD, 0);
    ok = accepted_peer_len == sizeof(accepted_peer) && accepted_peer.sin_family == AF_INET &&
         accepted_peer.sin_addr.s_addr == integration_ip4(127, 0, 0, 1) && accepted_peer.sin_port != 0 &&
         (status_flags & O_NONBLOCK) != 0 && (fd_flags & FD_CLOEXEC) != 0;
  }

  const char msg[] = "tcp-hi";
  char got[16] = {0};
  const char reply[] = "tcp-ok";
  char reply_got[16] = {0};
  if (ok) {
    ok = send(client, msg, sizeof(msg) - 1, 0) == (ssize_t)(sizeof(msg) - 1) &&
         recv(accepted, got, sizeof(got) - 1, 0) == (ssize_t)(sizeof(msg) - 1) && strcmp(got, msg) == 0 &&
         send(accepted, reply, sizeof(reply) - 1, 0) == (ssize_t)(sizeof(reply) - 1) &&
         recv(client, reply_got, sizeof(reply_got) - 1, 0) == (ssize_t)(sizeof(reply) - 1) &&
         strcmp(reply_got, reply) == 0;
  }

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] tcp loopback accept: %s (%s/%s)\n", ok ? "PASS" : "FAIL", got, reply_got);
  return ok;
}

static int tcp_bulk_send_regression(void) {
  enum { BULK_LEN = 65536 };
  int client = -1;
  int accepted = -1;
  if (!open_tcp_loopback_pair(45694, &client, &accepted)) {
    printf("[spore] tcp bulk send: FAIL pair\n");
    return 0;
  }

  char *send_buf = malloc(BULK_LEN);
  char *recv_buf = malloc(BULK_LEN);
  if (send_buf == NULL || recv_buf == NULL) {
    free(send_buf);
    free(recv_buf);
    close(client);
    close(accepted);
    printf("[spore] tcp bulk send: FAIL alloc\n");
    return 0;
  }
  for (size_t i = 0; i < BULK_LEN; ++i) {
    send_buf[i] = (char)('a' + (i % 26));
    recv_buf[i] = 0;
  }

  ssize_t first = send(client, send_buf, BULK_LEN, 0);
  size_t received = 0;
  while (first > 0 && received < (size_t)first) {
    ssize_t n = recv(accepted, recv_buf + received, (size_t)first - received, 0);
    if (n <= 0) { break; }
    received += (size_t)n;
  }
  int ok = first > 1460 && received == (size_t)first && memcmp(send_buf, recv_buf, received) == 0;

  free(send_buf);
  free(recv_buf);
  close(client);
  close(accepted);
  int msg_client = -1;
  int msg_accepted = -1;
  ssize_t msg_first = -1;
  size_t msg_received = 0;
  if (ok && open_tcp_loopback_pair(45695, &msg_client, &msg_accepted)) {
    enum { MSG_BULK_LEN = 32768 };
    char *msg_send = malloc(MSG_BULK_LEN);
    char *msg_recv = malloc(MSG_BULK_LEN);
    ok = msg_send != NULL && msg_recv != NULL;
    if (ok) {
      for (size_t i = 0; i < MSG_BULK_LEN; ++i) {
        msg_send[i] = (char)('A' + (i % 26));
        msg_recv[i] = 0;
      }
      struct iovec iov[2] = {
        {.iov_base = msg_send, .iov_len = MSG_BULK_LEN / 2},
        {.iov_base = msg_send + MSG_BULK_LEN / 2, .iov_len = MSG_BULK_LEN / 2},
      };
      struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
      msg_first = sendmsg(msg_client, &msg, 0);
      while (msg_first > 0 && msg_received < (size_t)msg_first) {
        ssize_t n = recv(msg_accepted, msg_recv + msg_received, (size_t)msg_first - msg_received, 0);
        if (n <= 0) { break; }
        msg_received += (size_t)n;
      }
      ok = msg_first > 1460 && msg_received == (size_t)msg_first && memcmp(msg_send, msg_recv, msg_received) == 0;
    }
    free(msg_send);
    free(msg_recv);
  } else {
    ok = 0;
  }
  if (msg_client >= 0) { close(msg_client); }
  if (msg_accepted >= 0) { close(msg_accepted); }

  printf("[spore] tcp bulk send: %s send=%ld/%zu sendmsg=%ld/%zu\n", ok ? "PASS" : "FAIL", (long)first, received,
         (long)msg_first, msg_received);
  return ok;
}

static int tcp_fin_eof_regression(void) {
  int client = -1;
  int accepted = -1;
  if (!open_tcp_loopback_pair(45686, &client, &accepted)) {
    printf("[spore] tcp FIN EOF: FAIL pair\n");
    return 0;
  }

  const char msg[] = "fin-data";
  char got[16] = {0};
  int ok = send(accepted, msg, sizeof(msg) - 1, 0) == (ssize_t)(sizeof(msg) - 1) &&
           recv(client, got, sizeof(got) - 1, 0) == (ssize_t)(sizeof(msg) - 1) && strcmp(got, msg) == 0;

  ok = ok && close(accepted) == 0;

  struct pollfd pfd = {.fd = client, .events = POLLIN};
  int poll_rc = poll(&pfd, 1, 0);
  char byte = 0;
  errno = 0;
  ssize_t eof = recv(client, &byte, sizeof(byte), MSG_DONTWAIT);
  int recv_errno = errno;

  close(client);

  ok = ok && poll_rc == 1 && (pfd.revents & POLLIN) != 0 && eof == 0;
  printf("[spore] tcp FIN EOF: %s poll=0x%x eof=%zd errno=%d\n", ok ? "PASS" : "FAIL", pfd.revents, eof, recv_errno);
  return ok;
}

static int tcp_refused_poll_error_regression(void) {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    printf("[spore] tcp refused pollerr: FAIL socket\n");
    return 0;
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) { (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK); }

  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), 45991);
  errno = 0;
  int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
  int connect_errno = errno;

  struct pollfd pfd = {.fd = fd, .events = 0};
  int poll_rc = poll(&pfd, 1, 50);

  struct sockaddr_in refused_peer;
  socklen_t refused_peer_len = sizeof(refused_peer);
  errno = 0;
  int peer_rc = getpeername(fd, (struct sockaddr *)&refused_peer, &refused_peer_len);
  int peer_errno = errno;

  int epoll_fd = epoll_create1(0);
  int epoll_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int epoll_connect_errno = 0;
  int epoll_rc = -1;
  uint32_t epoll_events = 0;
  if (epoll_fd >= 0 && epoll_sock >= 0) {
    int epoll_flags = fcntl(epoll_sock, F_GETFL, 0);
    if (epoll_flags >= 0) { (void)fcntl(epoll_sock, F_SETFL, epoll_flags | O_NONBLOCK); }
    errno = 0;
    (void)connect(epoll_sock, (struct sockaddr *)&sa, sizeof(sa));
    epoll_connect_errno = errno;
    struct epoll_event ev = {.events = 0, .data.u32 = 0x66};
    struct epoll_event out = {0};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, epoll_sock, &ev) == 0) {
      epoll_rc = epoll_wait(epoll_fd, &out, 1, 50);
      epoll_events = out.events;
    }
  }
  if (epoll_sock >= 0) { close(epoll_sock); }
  if (epoll_fd >= 0) { close(epoll_fd); }

  int so_error = 0;
  socklen_t so_error_len = sizeof(so_error);
  int get_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
  int after_error = -1;
  so_error_len = sizeof(after_error);
  int get_after_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &after_error, &so_error_len);

  close(fd);

  int ok = rc < 0 && (connect_errno == EINPROGRESS || connect_errno == ECONNREFUSED) && poll_rc == 1 &&
           (pfd.revents & POLLERR) != 0 && get_rc == 0 && so_error == ECONNREFUSED && get_after_rc == 0 &&
           after_error == 0 && peer_rc < 0 && peer_errno == ENOTCONN &&
           (epoll_connect_errno == EINPROGRESS || epoll_connect_errno == ECONNREFUSED) && epoll_rc == 1 &&
           (epoll_events & EPOLLERR) != 0;
  printf("[spore] tcp refused pollerr: %s connect=%d poll=0x%x epoll=0x%x so_error=%d after=%d peer=%d\n",
         ok ? "PASS" : "FAIL", connect_errno, pfd.revents, epoll_events, so_error, after_error, peer_errno);
  return ok;
}

static int tcp_send_timeout_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] tcp send timeout: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), 45682);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
  ok = ok && setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;

  char payload[1400];
  memset(payload, 'x', sizeof(payload));
  int saw_eagain = 0;
  int poll_blocked = 0;
  uint64_t elapsed_us = 0;
  if (ok) {
    for (int i = 0; i < 260; ++i) {
      errno = 0;
      uint64_t start = integration_usec_now();
      ssize_t n = send(client, payload, sizeof(payload), 0);
      uint64_t end = integration_usec_now();
      if (n < 0) {
        saw_eagain = errno == EAGAIN || errno == EWOULDBLOCK;
        elapsed_us = end - start;
        break;
      }
      if (n <= 0) {
        ok = 0;
        break;
      }
    }
    struct pollfd pfd = {.fd = client, .events = POLLOUT};
    poll_blocked = poll(&pfd, 1, 0) == 0;
  }

  ok = ok && saw_eagain && poll_blocked && elapsed_us >= 5000;
  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] tcp send timeout: %s elapsed=%luus poll_blocked=%d\n", ok ? "PASS" : "FAIL",
         (unsigned long)elapsed_us, poll_blocked);
  return ok;
}

static int tcp_sendmsg_timeout_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] tcp sendmsg timeout: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, integration_ip4(127, 0, 0, 1), 45683);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
  ok = ok && setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;

  char first[700];
  char second[700];
  memset(first, 'a', sizeof(first));
  memset(second, 'b', sizeof(second));
  struct iovec iov[2] = {
    {.iov_base = first, .iov_len = sizeof(first)},
    {.iov_base = second, .iov_len = sizeof(second)},
  };
  struct msghdr msg = {
    .msg_iov = iov,
    .msg_iovlen = 2,
  };
  int saw_eagain = 0;
  uint64_t elapsed_us = 0;
  if (ok) {
    for (int i = 0; i < 260; ++i) {
      errno = 0;
      uint64_t start = integration_usec_now();
      ssize_t n = sendmsg(client, &msg, 0);
      uint64_t end = integration_usec_now();
      if (n < 0) {
        saw_eagain = errno == EAGAIN || errno == EWOULDBLOCK;
        elapsed_us = end - start;
        break;
      }
      if (n <= 0) {
        ok = 0;
        break;
      }
    }
  }

  ok = ok && saw_eagain && elapsed_us >= 5000;
  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] tcp sendmsg timeout: %s elapsed=%luus\n", ok ? "PASS" : "FAIL", (unsigned long)elapsed_us);
  return ok;
}

static int getpeername_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] getpeername: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in peer;
  integration_fill_udp_addr(&peer, integration_ip4(10, 0, 2, 2), 5555);
  struct sockaddr_in got;
  socklen_t got_len = sizeof(got);
  int ok = connect(fd, (struct sockaddr *)&peer, sizeof(peer)) == 0 &&
           getpeername(fd, (struct sockaddr *)&got, &got_len) == 0 && got.sin_port == peer.sin_port &&
           got.sin_addr.s_addr == peer.sin_addr.s_addr && got_len == sizeof(got);
  close(fd);
  printf("[spore] getpeername: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

int integration_udp_send_target(uint32_t ip, uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) { return -1; }
  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, ip, port);
  const char msg[] = "egress";
  int rc = (int)sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  close(fd);
  return rc;
}

int integration_udp_connect_send(uint32_t connect_ip, uint16_t connect_port, uint32_t send_ip, uint16_t send_port,
                                 int null_dest) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) { return -1; }
  struct sockaddr_in sa;
  integration_fill_udp_addr(&sa, connect_ip, connect_port);
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  const char msg[] = "spoof";
  int rc;
  if (null_dest) {
    rc = (int)sendto(fd, msg, sizeof(msg) - 1, 0, NULL, 0);
  } else {
    integration_fill_udp_addr(&sa, send_ip, send_port);
    rc = (int)sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  }
  close(fd);
  return rc;
}

int integration_socket_suite(void) {
  return udp_bind_conflict_regression() && udp_receive_queue_regression() && udp_msg_iov_regression() &&
         udp_recvmsg_trunc_regression() && socket_msg_peek_regression() && socket_msg_waitall_regression() &&
         getsockname_regression() && socket_options_regression() && tcp_rcvlowat_poll_regression() &&
         tcp_abortive_linger_regression() && udp_broadcast_option_regression() && udp_loopback_refused_regression() &&
         udp_disconnect_regression() && udp_external_refused_regression() && socket_timeout_regression() &&
         socket_msg_dontwait_regression() && socket_shutdown_regression() && tcp_sigpipe_regression() &&
         tcp_loopback_accept_regression() && tcp_bulk_send_regression() && tcp_fin_eof_regression() &&
         tcp_refused_poll_error_regression() && tcp_send_timeout_regression() && tcp_sendmsg_timeout_regression() &&
         getpeername_regression();
}
