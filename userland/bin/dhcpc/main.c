#include <spore.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
  DHCP_CLIENT_PORT = 68,
  DHCP_SERVER_PORT = 67,
  DHCP_DISCOVER = 1,
  DHCP_OFFER = 2,
  DHCP_REQUEST = 3,
  DHCP_ACK = 5,
  DHCP_MAGIC = 0x63825363u,
};

static uint16_t be16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static uint32_t get_le_ip(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t build_packet(uint8_t *pkt, size_t cap, uint8_t type, uint32_t xid, uint32_t requested, uint32_t server) {
  if (cap < 300) { return 0; }
  memset(pkt, 0, cap);
  pkt[0] = 1;
  pkt[1] = 1;
  pkt[2] = 6;
  put_be32(pkt + 4, xid);
  pkt[10] = 0x80;
  pkt[28] = 0x52;
  pkt[29] = 0x54;
  pkt[30] = 0x00;
  pkt[31] = 0x12;
  pkt[32] = 0x34;
  pkt[33] = 0x56;
  put_be32(pkt + 236, DHCP_MAGIC);
  size_t o = 240;
  pkt[o++] = 53;
  pkt[o++] = 1;
  pkt[o++] = type;
  if (requested != 0) {
    pkt[o++] = 50;
    pkt[o++] = 4;
    pkt[o++] = (uint8_t)requested;
    pkt[o++] = (uint8_t)(requested >> 8);
    pkt[o++] = (uint8_t)(requested >> 16);
    pkt[o++] = (uint8_t)(requested >> 24);
  }
  if (server != 0) {
    pkt[o++] = 54;
    pkt[o++] = 4;
    pkt[o++] = (uint8_t)server;
    pkt[o++] = (uint8_t)(server >> 8);
    pkt[o++] = (uint8_t)(server >> 16);
    pkt[o++] = (uint8_t)(server >> 24);
  }
  pkt[o++] = 55;
  pkt[o++] = 3;
  pkt[o++] = 1;
  pkt[o++] = 3;
  pkt[o++] = 6;
  pkt[o++] = 255;
  return o;
}

struct lease {
  uint8_t msg_type;
  uint32_t yiaddr;
  uint32_t server;
  uint32_t netmask;
  uint32_t router;
  uint32_t dns;
};

static bool parse_reply(const uint8_t *pkt, size_t len, uint32_t xid, struct lease *lease) {
  if (len < 240 || pkt[0] != 2 || pkt[4] != (uint8_t)(xid >> 24) || pkt[5] != (uint8_t)(xid >> 16) ||
      pkt[6] != (uint8_t)(xid >> 8) || pkt[7] != (uint8_t)xid) {
    return false;
  }
  memset(lease, 0, sizeof(*lease));
  lease->yiaddr = get_le_ip(pkt + 16);
  size_t o = 240;
  while (o < len) {
    uint8_t opt = pkt[o++];
    if (opt == 255) { break; }
    if (opt == 0) { continue; }
    if (o >= len) { break; }
    uint8_t opt_len = pkt[o++];
    if (o + opt_len > len) { break; }
    if (opt == 53 && opt_len >= 1) { lease->msg_type = pkt[o]; }
    if (opt == 54 && opt_len >= 4) { lease->server = get_le_ip(pkt + o); }
    if (opt == 1 && opt_len >= 4) { lease->netmask = get_le_ip(pkt + o); }
    if (opt == 3 && opt_len >= 4) { lease->router = get_le_ip(pkt + o); }
    if (opt == 6 && opt_len >= 4) { lease->dns = get_le_ip(pkt + o); }
    o += opt_len;
  }
  return lease->msg_type != 0 && lease->yiaddr != 0;
}

static bool recv_lease(int fd, uint32_t xid, uint8_t want, struct lease *lease) {
  for (;;) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 2500) <= 0) { return false; }
    uint8_t buf[600];
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
    if (n > 0 && parse_reply(buf, (size_t)n, xid, lease) && lease->msg_type == want) { return true; }
  }
}

static bool write_resolv(uint32_t dns) {
  char ip[32];
  format_ipv4(dns, ip, sizeof(ip));
  FILE *f = fopen("/etc/resolv.conf", "w");
  if (f == NULL) { return false; }
  fprintf(f, "nameserver %s\n", ip);
  fclose(f);
  return true;
}

int main(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }

  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_port = be16(DHCP_CLIENT_PORT);
  if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
    perror("bind");
    close(fd);
    return EXIT_FAILURE;
  }

  struct sockaddr_in dst;
  memset(&dst, 0, sizeof(dst));
  dst.sin_family = AF_INET;
  dst.sin_port = be16(DHCP_SERVER_PORT);
  dst.sin_addr.s_addr = 0xffffffffu;

  uint32_t xid = 0x53504f52u;
  uint8_t pkt[320];
  size_t len = build_packet(pkt, sizeof(pkt), DHCP_DISCOVER, xid, 0, 0);
  if (sendto(fd, pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
    perror("sendto");
    close(fd);
    return EXIT_FAILURE;
  }

  struct lease offer;
  if (!recv_lease(fd, xid, DHCP_OFFER, &offer)) {
    fputs("dhcpc: no offer\n", stderr);
    close(fd);
    return EXIT_FAILURE;
  }

  len = build_packet(pkt, sizeof(pkt), DHCP_REQUEST, xid, offer.yiaddr, offer.server);
  if (sendto(fd, pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
    perror("sendto");
    close(fd);
    return EXIT_FAILURE;
  }

  struct lease ack;
  if (!recv_lease(fd, xid, DHCP_ACK, &ack)) {
    fputs("dhcpc: no ack\n", stderr);
    close(fd);
    return EXIT_FAILURE;
  }
  close(fd);

  if (ack.router == 0) { ack.router = offer.server; }
  if (ack.dns == 0) { ack.dns = offer.server; }
  if (ack.netmask == 0) { ack.netmask = 0x00ffffffu; }

  struct net_config cfg = {
    .local_ip = ack.yiaddr,
    .gateway_ip = ack.router,
    .netmask = ack.netmask,
    .dns_ip = ack.dns,
    .configured = 1,
  };
  if (!net_config_set(&cfg)) {
    perror("spore_net_config");
    return EXIT_FAILURE;
  }
  (void)write_resolv(ack.dns);
  char ip[32];
  char gw[32];
  char dns[32];
  format_ipv4(cfg.local_ip, ip, sizeof(ip));
  format_ipv4(cfg.gateway_ip, gw, sizeof(gw));
  format_ipv4(cfg.dns_ip, dns, sizeof(dns));
  printf("leased %s gateway %s dns %s\n", ip, gw, dns);
  return EXIT_SUCCESS;
}
