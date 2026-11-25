#include "integration.h"
#include <arpa/inet.h>
#include <errno.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>

static int configure(int fd, struct nlmsghdr *h) {
  struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
  h->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
  if (sendto(fd, h, h->nlmsg_len, 0, (struct sockaddr *)&kernel, sizeof(kernel)) != h->nlmsg_len) { return 0; }
  struct { struct nlmsghdr h; struct nlmsgerr body; } reply;
  return recv(fd, &reply, sizeof(reply), 0) == sizeof(reply) &&
         reply.h.nlmsg_type == NLMSG_ERROR && reply.h.nlmsg_seq == h->nlmsg_seq && !reply.body.error;
}

static int start_loopback(void) {
  int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE);
  unsigned index = if_nametoindex("lo");
  if (fd < 0 || !index || if_nametoindex("eth0")) { return 0; }
  struct { struct nlmsghdr h; struct ifaddrmsg a; struct rtattr attr; uint32_t ip; } address = {
    .h = {.nlmsg_type = RTM_NEWADDR, .nlmsg_len = sizeof(address), .nlmsg_seq = 1, .nlmsg_flags = NLM_F_CREATE},
    .a = {.ifa_family = AF_INET, .ifa_prefixlen = 8, .ifa_index = index, .ifa_scope = RT_SCOPE_HOST},
    .attr = {.rta_type = IFA_LOCAL, .rta_len = RTA_LENGTH(4)}, .ip = htonl(INADDR_LOOPBACK)};
  struct { struct nlmsghdr h; struct ifinfomsg link; } up = {
    .h = {.nlmsg_type = RTM_NEWLINK, .nlmsg_len = sizeof(up), .nlmsg_seq = 2},
    .link = {.ifi_index = index, .ifi_change = IFF_UP, .ifi_flags = IFF_UP}};
  int ok = configure(fd, &address.h) && configure(fd, &up.h);
  close(fd);
  return ok;
}

static struct sockaddr_in addr(unsigned port) {
  return (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
}

static int receive_byte(int fd, char expected) {
  struct pollfd p = {.fd = fd, .events = POLLIN};
  char c;
  return poll(&p, 1, 1000) == 1 && recv(fd, &c, 1, MSG_DONTWAIT) == 1 && c == expected;
}

static int private_network(int inherited, struct sockaddr_in a) {
  if (unshare(CLONE_NEWNET)) { return 1; }
  int udp = socket(AF_INET, SOCK_DGRAM, 0);
  if (udp < 0 || sendto(udp, "x", 1, 0, (struct sockaddr *)&a, sizeof(a)) != -1 || errno != ENETUNREACH) { return 2; }
  if (!start_loopback()) { return 3; }
  int tcp = socket(AF_INET, SOCK_STREAM, 0);
  if (tcp < 0 || connect(tcp, (struct sockaddr *)&a, sizeof(a)) != -1 || errno != ECONNREFUSED) { return 4; }
  close(tcp);
  if (bind(udp, (struct sockaddr *)&a, sizeof(a)) ||
      sendto(udp, "i", 1, 0, (struct sockaddr *)&a, sizeof(a)) != 1 || !receive_byte(udp, 'i')) { return 5; }
  if (sendto(inherited, "o", 1, 0, (struct sockaddr *)&a, sizeof(a)) != 1) { return 6; }
  tcp = socket(AF_INET, SOCK_STREAM, 0);
  int client = socket(AF_INET, SOCK_STREAM, 0);
  if (tcp < 0 || client < 0 || bind(tcp, (struct sockaddr *)&a, sizeof(a)) || listen(tcp, 1) ||
      connect(client, (struct sockaddr *)&a, sizeof(a))) { return 7; }
  int peer = accept(tcp, NULL, NULL);
  if (peer < 0 || send(client, "t", 1, 0) != 1 || !receive_byte(peer, 't')) { return 8; }
  a.sin_addr.s_addr = htonl(0xc0000201u);
  if (sendto(udp, "x", 1, 0, (struct sockaddr *)&a, sizeof(a)) != -1 || errno != ENETUNREACH) { return 9; }
  return 0;
}

static int unix_datagrams(void) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair)) { return 0; }
  int ok = write(pair[0], "a", 1) == 1 && receive_byte(pair[1], 'a');
  close(pair[0]);
  close(pair[1]);
  int sender = socket(AF_UNIX, SOCK_DGRAM, 0), receiver = socket(AF_UNIX, SOCK_DGRAM, 0);
  struct sockaddr_un a = {.sun_family = AF_UNIX, .sun_path = "/tmp/namespace-dgram-a"};
  struct sockaddr_un b = {.sun_family = AF_UNIX, .sun_path = "/tmp/namespace-dgram-b"};
  struct sockaddr_un from = {0};
  socklen_t size = sizeof(from);
  char c;
  ok = ok && sender >= 0 && receiver >= 0 && !bind(sender, (struct sockaddr *)&a, sizeof(a)) &&
       !bind(receiver, (struct sockaddr *)&b, sizeof(b)) && !connect(sender, (struct sockaddr *)&b, sizeof(b)) &&
       write(sender, "b", 1) == 1 &&
       recvfrom(receiver, &c, 1, MSG_DONTWAIT, (struct sockaddr *)&from, &size) == 1 && c == 'b' &&
       from.sun_family == AF_UNIX && !strcmp(from.sun_path, a.sun_path);
  if (sender >= 0) { close(sender); }
  if (receiver >= 0) { close(receiver); }
  unlink(a.sun_path);
  unlink(b.sun_path);
  return ok;
}

int integration_network_namespace(void) {
  struct sockaddr_in a = addr(37231);
  int udp = socket(AF_INET, SOCK_DGRAM, 0);
  int tcp = socket(AF_INET, SOCK_STREAM, 0);
  if (udp < 0 || tcp < 0 || bind(udp, (struct sockaddr *)&a, sizeof(a)) ||
      bind(tcp, (struct sockaddr *)&a, sizeof(a)) || listen(tcp, 1)) { return 0; }
  pid_t child = fork();
  if (!child) { _exit(private_network(udp, a)); }
  int status = -1;
  int ok = child > 0 && waitpid(child, &status, 0) == child && !status && receive_byte(udp, 'o');
  struct pollfd p = {.fd = tcp, .events = POLLIN};
  ok &= !poll(&p, 1, 0);
  ok &= unix_datagrams();
  close(tcp);
  close(udp);
  printf("[spore] private loopback TCP, UDP and inherited socket: %s status=%d\n", ok ? "PASS" : "FAIL", status);
  return ok;
}
