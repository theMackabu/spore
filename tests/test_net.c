#include "net.h"
#include "virtio_net.h"

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  ETH_TYPE_ARP = 0x0806,
  ETH_TYPE_IPV4 = 0x0800,
  ARP_REQUEST = 1,
  ARP_REPLY = 2,
  ETH_HEADER_LEN = 14,
  IPV4_HEADER_LEN = 20,
  ICMP_HEADER_LEN = 8,
  UDP_HEADER_LEN = 8,
  TCP_HEADER_LEN = 20,
  ICMP_DEST_UNREACH = 3,
  ICMP_CODE_PORT_UNREACH = 3,
  ICMP_ECHO_REPLY = 0,
  ICMP_ECHO_REQUEST = 8,
  TCP_FLAG_FIN = 0x01,
  TCP_FLAG_SYN = 0x02,
  TCP_FLAG_RST = 0x04,
  IPV4_FLAG_MORE_FRAGMENTS = 0x2000,
  IPV4_FRAGMENT_OFFSET_MASK = 0x1fff,
};

static bool saw_udp_error;
static bool saw_tcp_error;
static uint32_t saw_remote_ip;
static uint16_t saw_remote_port;
static uint16_t saw_local_port;
static int saw_error;

static uint16_t load_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t load_ip(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static void store_ip(uint8_t *p, uint32_t ip) {
  p[0] = (uint8_t)ip;
  p[1] = (uint8_t)(ip >> 8);
  p[2] = (uint8_t)(ip >> 16);
  p[3] = (uint8_t)(ip >> 24);
}

static uint16_t ip_checksum(const void *data, size_t len) {
  const uint8_t *p = data;
  uint32_t sum = 0;
  while (len >= 2) {
    sum += load_be16(p);
    p += 2;
    len -= 2;
  }
  if (len != 0) { sum += (uint32_t)p[0] << 8; }
  while ((sum >> 16) != 0) {
    sum = (sum & 0xffffu) + (sum >> 16);
  }
  return (uint16_t)~sum;
}

static uint16_t ip_payload_checksum(uint8_t proto, uint32_t src_ip, uint32_t dst_ip, const void *payload, size_t len) {
  uint8_t pseudo[12 + TCP_HEADER_LEN];
  assert(len <= sizeof(pseudo) - 12);
  store_ip(pseudo, src_ip);
  store_ip(pseudo + 4, dst_ip);
  pseudo[8] = 0;
  pseudo[9] = proto;
  store_be16(pseudo + 10, (uint16_t)len);
  for (size_t i = 0; i < len; ++i) { pseudo[12 + i] = ((const uint8_t *)payload)[i]; }
  return ip_checksum(pseudo, 12 + len);
}

static void write_ipv4_header(uint8_t *ip, uint16_t total_len, uint8_t proto, uint32_t src, uint32_t dst) {
  ip[0] = 0x45;
  ip[1] = 0;
  store_be16(ip + 2, total_len);
  store_be16(ip + 4, 1);
  store_be16(ip + 6, 0);
  ip[8] = 64;
  ip[9] = proto;
  store_be16(ip + 10, 0);
  store_ip(ip + 12, src);
  store_ip(ip + 16, dst);
  store_be16(ip + 10, ip_checksum(ip, IPV4_HEADER_LEN));
}

static void write_ipv4_fragment_field(uint8_t *ip, uint16_t fragment) {
  store_be16(ip + 6, fragment);
  store_be16(ip + 10, 0);
  store_be16(ip + 10, ip_checksum(ip, IPV4_HEADER_LEN));
}

static void rewrite_ipv4_source(uint8_t *ip, uint32_t src_ip) {
  store_ip(ip + 12, src_ip);
  store_be16(ip + 10, 0);
  store_be16(ip + 10, ip_checksum(ip, IPV4_HEADER_LEN));
}

static void rewrite_ipv4_dest(uint8_t *ip, uint32_t dst_ip) {
  store_ip(ip + 16, dst_ip);
  store_be16(ip + 10, 0);
  store_be16(ip + 10, ip_checksum(ip, IPV4_HEADER_LEN));
}

static void rewrite_ipv4_ttl(uint8_t *ip, uint8_t ttl) {
  ip[8] = ttl;
  store_be16(ip + 10, 0);
  store_be16(ip + 10, ip_checksum(ip, IPV4_HEADER_LEN));
}

static void write_udp_checksum(uint8_t *udp, uint16_t sum) {
  store_be16(udp + 6, sum);
}

static void rewrite_udp_ports(uint8_t *frame, uint16_t src_port, uint16_t dst_port) {
  uint8_t *udp = frame + ETH_HEADER_LEN + IPV4_HEADER_LEN;
  store_be16(udp, src_port);
  store_be16(udp + 2, dst_port);
}

static size_t make_icmp_port_unreachable(uint8_t *frame, uint32_t remote_ip, uint16_t remote_port,
                                         uint16_t local_port) {
  const uint8_t local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  const uint8_t remote_mac[6] = {0x52, 0x55, 0x00, 0xaa, 0xbb, 0xcc};
  uint32_t local_ip = net_ipv4(10, 0, 2, 15);

  for (size_t i = 0; i < 6; ++i) {
    frame[i] = local_mac[i];
    frame[6 + i] = remote_mac[i];
  }
  store_be16(frame + 12, ETH_TYPE_IPV4);

  uint8_t *outer_ip = frame + ETH_HEADER_LEN;
  uint8_t *icmp = outer_ip + IPV4_HEADER_LEN;
  uint8_t *quoted_ip = icmp + ICMP_HEADER_LEN;
  uint8_t *quoted_udp = quoted_ip + IPV4_HEADER_LEN;

  write_ipv4_header(quoted_ip, IPV4_HEADER_LEN + UDP_HEADER_LEN, NET_IP_UDP, local_ip, remote_ip);
  store_be16(quoted_udp, local_port);
  store_be16(quoted_udp + 2, remote_port);
  store_be16(quoted_udp + 4, UDP_HEADER_LEN);
  store_be16(quoted_udp + 6, 0);

  icmp[0] = ICMP_DEST_UNREACH;
  icmp[1] = ICMP_CODE_PORT_UNREACH;
  store_be16(icmp + 2, 0);
  store_be16(icmp + 4, 0);
  store_be16(icmp + 6, 0);
  size_t icmp_len = ICMP_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN;
  store_be16(icmp + 2, ip_checksum(icmp, icmp_len));

  size_t outer_len = IPV4_HEADER_LEN + icmp_len;
  write_ipv4_header(outer_ip, (uint16_t)outer_len, NET_IP_ICMP, remote_ip, local_ip);
  return ETH_HEADER_LEN + outer_len;
}

static size_t make_icmp_tcp_port_unreachable(uint8_t *frame, uint32_t remote_ip, uint16_t remote_port,
                                             uint16_t local_port) {
  size_t len = make_icmp_port_unreachable(frame, remote_ip, remote_port, local_port);
  uint8_t *outer_ip = frame + ETH_HEADER_LEN;
  uint8_t *quoted_ip = outer_ip + IPV4_HEADER_LEN + ICMP_HEADER_LEN;
  quoted_ip[9] = NET_IP_TCP;
  store_be16(quoted_ip + 10, 0);
  store_be16(quoted_ip + 10, ip_checksum(quoted_ip, IPV4_HEADER_LEN));
  uint8_t *icmp = outer_ip + IPV4_HEADER_LEN;
  store_be16(icmp + 2, 0);
  store_be16(icmp + 2, ip_checksum(icmp, ICMP_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN));
  return len;
}

static void rewrite_quoted_source_ip(uint8_t *frame, uint32_t src_ip) {
  uint8_t *outer_ip = frame + ETH_HEADER_LEN;
  uint8_t *quoted_ip = outer_ip + IPV4_HEADER_LEN + ICMP_HEADER_LEN;
  uint8_t *icmp = outer_ip + IPV4_HEADER_LEN;
  store_ip(quoted_ip + 12, src_ip);
  store_be16(quoted_ip + 10, 0);
  store_be16(quoted_ip + 10, ip_checksum(quoted_ip, IPV4_HEADER_LEN));
  store_be16(icmp + 2, 0);
  store_be16(icmp + 2, ip_checksum(icmp, ICMP_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN));
}

static void corrupt_quoted_ip_checksum(uint8_t *frame) {
  uint8_t *outer_ip = frame + ETH_HEADER_LEN;
  uint8_t *quoted_ip = outer_ip + IPV4_HEADER_LEN + ICMP_HEADER_LEN;
  uint8_t *icmp = outer_ip + IPV4_HEADER_LEN;
  quoted_ip[10] ^= 0xff;
  store_be16(icmp + 2, 0);
  store_be16(icmp + 2, ip_checksum(icmp, ICMP_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN));
}

static size_t make_arp_reply(uint8_t *frame, uint32_t remote_ip) {
  const uint8_t local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  const uint8_t remote_mac[6] = {0x52, 0x55, 0x00, 0xaa, 0xbb, 0xcc};
  uint32_t local_ip = net_ipv4(10, 0, 2, 15);

  for (size_t i = 0; i < 6; ++i) {
    frame[i] = local_mac[i];
    frame[6 + i] = remote_mac[i];
  }
  store_be16(frame + 12, ETH_TYPE_ARP);

  uint8_t *arp = frame + ETH_HEADER_LEN;
  store_be16(arp, 1);
  store_be16(arp + 2, ETH_TYPE_IPV4);
  arp[4] = 6;
  arp[5] = 4;
  store_be16(arp + 6, ARP_REPLY);
  for (size_t i = 0; i < 6; ++i) {
    arp[8 + i] = remote_mac[i];
    arp[18 + i] = local_mac[i];
  }
  store_ip(arp + 14, remote_ip);
  store_ip(arp + 24, local_ip);
  return ETH_HEADER_LEN + 28;
}

static void rewrite_arp_sender_ip(uint8_t *frame, uint32_t sender_ip) {
  uint8_t *arp = frame + ETH_HEADER_LEN;
  store_ip(arp + 14, sender_ip);
}

static void rewrite_arp_target_ip(uint8_t *frame, uint32_t target_ip) {
  uint8_t *arp = frame + ETH_HEADER_LEN;
  store_ip(arp + 24, target_ip);
}

static size_t make_udp_to_closed_port(uint8_t *frame, uint32_t remote_ip, uint16_t remote_port, uint16_t local_port) {
  const uint8_t local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  const uint8_t remote_mac[6] = {0x52, 0x55, 0x00, 0xaa, 0xbb, 0xcc};
  uint32_t local_ip = net_ipv4(10, 0, 2, 15);

  for (size_t i = 0; i < 6; ++i) {
    frame[i] = local_mac[i];
    frame[6 + i] = remote_mac[i];
  }
  store_be16(frame + 12, ETH_TYPE_IPV4);

  uint8_t *ip = frame + ETH_HEADER_LEN;
  uint8_t *udp = ip + IPV4_HEADER_LEN;
  store_be16(udp, remote_port);
  store_be16(udp + 2, local_port);
  store_be16(udp + 4, UDP_HEADER_LEN);
  store_be16(udp + 6, 0);
  write_ipv4_header(ip, IPV4_HEADER_LEN + UDP_HEADER_LEN, NET_IP_UDP, remote_ip, local_ip);
  return ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN;
}

static size_t make_tcp_to_broadcast(uint8_t *frame, uint32_t checksum_dst_ip) {
  const uint8_t local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  const uint8_t remote_mac[6] = {0x52, 0x55, 0x00, 0xaa, 0xbb, 0xcc};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint32_t broadcast_ip = net_ipv4(255, 255, 255, 255);

  for (size_t i = 0; i < 6; ++i) {
    frame[i] = local_mac[i];
    frame[6 + i] = remote_mac[i];
  }
  store_be16(frame + 12, ETH_TYPE_IPV4);

  uint8_t *ip = frame + ETH_HEADER_LEN;
  uint8_t *tcp = ip + IPV4_HEADER_LEN;
  store_be16(tcp, 49153);
  store_be16(tcp + 2, 80);
  tcp[12] = (uint8_t)(5u << 4);
  tcp[13] = 0x02;
  store_be16(tcp + 14, 4096);
  store_be16(tcp + 16, 0);
  store_be16(tcp + 18, 0);
  store_be16(tcp + 16, ip_payload_checksum(NET_IP_TCP, remote_ip, checksum_dst_ip, tcp, TCP_HEADER_LEN));

  write_ipv4_header(ip, IPV4_HEADER_LEN + TCP_HEADER_LEN, NET_IP_TCP, remote_ip, broadcast_ip);
  return ETH_HEADER_LEN + IPV4_HEADER_LEN + TCP_HEADER_LEN;
}

static size_t make_tcp_to_local(uint8_t *frame, uint8_t flags) {
  const uint8_t local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  const uint8_t remote_mac[6] = {0x52, 0x55, 0x00, 0xaa, 0xbb, 0xcc};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint32_t local_ip = net_ipv4(10, 0, 2, 15);

  for (size_t i = 0; i < 6; ++i) {
    frame[i] = local_mac[i];
    frame[6 + i] = remote_mac[i];
  }
  store_be16(frame + 12, ETH_TYPE_IPV4);

  uint8_t *ip = frame + ETH_HEADER_LEN;
  uint8_t *tcp = ip + IPV4_HEADER_LEN;
  store_be16(tcp, 49153);
  store_be16(tcp + 2, 80);
  tcp[12] = (uint8_t)(5u << 4);
  tcp[13] = flags;
  store_be16(tcp + 14, 4096);
  store_be16(tcp + 16, 0);
  store_be16(tcp + 18, 0);
  store_be16(tcp + 16, ip_payload_checksum(NET_IP_TCP, remote_ip, local_ip, tcp, TCP_HEADER_LEN));

  write_ipv4_header(ip, IPV4_HEADER_LEN + TCP_HEADER_LEN, NET_IP_TCP, remote_ip, local_ip);
  return ETH_HEADER_LEN + IPV4_HEADER_LEN + TCP_HEADER_LEN;
}

static void rewrite_tcp_ports(uint8_t *frame, uint16_t src_port, uint16_t dst_port) {
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint32_t local_ip = net_ipv4(10, 0, 2, 15);
  uint8_t *tcp = frame + ETH_HEADER_LEN + IPV4_HEADER_LEN;
  store_be16(tcp, src_port);
  store_be16(tcp + 2, dst_port);
  store_be16(tcp + 16, 0);
  store_be16(tcp + 16, ip_payload_checksum(NET_IP_TCP, remote_ip, local_ip, tcp, TCP_HEADER_LEN));
}

static size_t make_icmp_to_broadcast(uint8_t *frame, uint8_t type) {
  const uint8_t local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  const uint8_t remote_mac[6] = {0x52, 0x55, 0x00, 0xaa, 0xbb, 0xcc};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint32_t broadcast_ip = net_ipv4(255, 255, 255, 255);

  for (size_t i = 0; i < 6; ++i) {
    frame[i] = local_mac[i];
    frame[6 + i] = remote_mac[i];
  }
  store_be16(frame + 12, ETH_TYPE_IPV4);

  uint8_t *ip = frame + ETH_HEADER_LEN;
  uint8_t *icmp = ip + IPV4_HEADER_LEN;
  icmp[0] = type;
  icmp[1] = 0;
  store_be16(icmp + 2, 0);
  store_be16(icmp + 4, 1);
  store_be16(icmp + 6, 1);
  store_be16(icmp + 2, ip_checksum(icmp, ICMP_HEADER_LEN));

  write_ipv4_header(ip, IPV4_HEADER_LEN + ICMP_HEADER_LEN, NET_IP_ICMP, remote_ip, broadcast_ip);
  return ETH_HEADER_LEN + IPV4_HEADER_LEN + ICMP_HEADER_LEN;
}

static bool saw_frame;
static uint8_t sent_frame[1514];
static uint32_t sent_frame_len;
static bool saw_tcp_segment;
static bool saw_icmp_delivery;

bool virtio_net_send_frame(const void *frame, uint32_t len) {
  assert(len <= sizeof(sent_frame));
  saw_frame = true;
  sent_frame_len = len;
  for (uint32_t i = 0; i < len; ++i) { sent_frame[i] = ((const uint8_t *)frame)[i]; }
  return true;
}

void virtio_net_poll(void) {}

bool virtio_net_init(uint64_t hhdm_offset) {
  (void)hhdm_offset;
  return true;
}

bool virtio_net_smoke_tx(void) {
  return true;
}

struct virtio_net_stats virtio_net_stats(void) {
  return (struct virtio_net_stats){0};
}

uint64_t cell_uptime_ticks(void) {
  return 1;
}

bool cell_egress_allowed(uint8_t proto, uint32_t ip, uint16_t port) {
  (void)proto;
  (void)ip;
  (void)port;
  return true;
}

bool cell_net_deliver_udp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const void *payload, size_t len) {
  (void)src_ip;
  (void)src_port;
  (void)dst_port;
  (void)payload;
  (void)len;
  return false;
}

bool cell_net_deliver_udp_error(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, int error) {
  saw_udp_error = true;
  saw_remote_ip = remote_ip;
  saw_remote_port = remote_port;
  saw_local_port = local_port;
  saw_error = error;
  return true;
}

bool cell_net_deliver_tcp_error(uint32_t remote_ip, uint16_t remote_port, uint16_t local_port, int error) {
  saw_tcp_error = true;
  saw_remote_ip = remote_ip;
  saw_remote_port = remote_port;
  saw_local_port = local_port;
  saw_error = error;
  return true;
}

void cell_net_deliver_tcp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint16_t window, uint8_t flags, const void *options, size_t options_len,
                          const void *payload, size_t len) {
  (void)src_ip;
  (void)src_port;
  (void)dst_port;
  (void)seq;
  (void)ack;
  (void)window;
  (void)flags;
  (void)options;
  (void)options_len;
  (void)payload;
  (void)len;
  saw_tcp_segment = true;
}

void cell_net_deliver_icmp(uint32_t src_ip, const void *payload, size_t len) {
  (void)src_ip;
  (void)payload;
  (void)len;
  saw_icmp_delivery = true;
}

void kputc(char c) {
  (void)c;
}

void kputs(const char *s) {
  (void)s;
}

void kprintf(const char *fmt, ...) {
  (void)fmt;
}

uint64_t klog_copy(char *dst, uint64_t cap) {
  (void)dst;
  (void)cap;
  return 0;
}

static void test_icmp_port_unreachable_maps_to_udp_socket_error(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + ICMP_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint16_t remote_port = 45994;
  uint16_t local_port = 49152;

  net_init();
  size_t len = make_icmp_port_unreachable(frame, remote_ip, remote_port, local_port);
  net_receive_ethernet(frame, len);

  assert(saw_udp_error);
  assert(saw_remote_ip == remote_ip);
  assert(saw_remote_port == remote_port);
  assert(saw_local_port == local_port);
  assert(saw_error == 111);
}

static void test_icmp_port_unreachable_maps_to_tcp_socket_error(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + ICMP_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint16_t remote_port = 45996;
  uint16_t local_port = 49154;

  net_init();
  saw_udp_error = false;
  saw_tcp_error = false;
  size_t len = make_icmp_tcp_port_unreachable(frame, remote_ip, remote_port, local_port);
  net_receive_ethernet(frame, len);

  assert(!saw_udp_error);
  assert(saw_tcp_error);
  assert(saw_remote_ip == remote_ip);
  assert(saw_remote_port == remote_port);
  assert(saw_local_port == local_port);
  assert(saw_error == 111);
}

static void test_icmp_error_requires_quoted_local_source(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + ICMP_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint16_t remote_port = 45996;
  uint16_t local_port = 49154;

  net_init();
  size_t len = make_icmp_port_unreachable(frame, remote_ip, remote_port, local_port);
  rewrite_quoted_source_ip(frame, net_ipv4(10, 0, 2, 44));
  saw_udp_error = false;
  saw_tcp_error = false;
  net_receive_ethernet(frame, len);

  assert(!saw_udp_error);
  assert(!saw_tcp_error);
}

static void test_icmp_error_requires_valid_quoted_ip_checksum(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + ICMP_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint16_t remote_port = 45994;
  uint16_t local_port = 49152;

  net_init();
  size_t len = make_icmp_port_unreachable(frame, remote_ip, remote_port, local_port);
  corrupt_quoted_ip_checksum(frame);
  saw_udp_error = false;
  saw_tcp_error = false;
  net_receive_ethernet(frame, len);

  assert(!saw_udp_error);
  assert(!saw_tcp_error);
}

static void test_udp_closed_port_sends_icmp_port_unreachable(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint8_t arp[ETH_HEADER_LEN + 28] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint32_t local_ip = net_ipv4(10, 0, 2, 15);
  uint16_t remote_port = 49153;
  uint16_t local_port = 45995;

  net_init();
  size_t arp_len = make_arp_reply(arp, remote_ip);
  net_receive_ethernet(arp, arp_len);

  saw_frame = false;
  size_t len = make_udp_to_closed_port(frame, remote_ip, remote_port, local_port);
  net_receive_ethernet(frame, len);

  assert(saw_frame);
  assert(sent_frame_len >= ETH_HEADER_LEN + IPV4_HEADER_LEN + ICMP_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN);
  const uint8_t *ip = sent_frame + ETH_HEADER_LEN;
  assert(load_ip(ip + 12) == local_ip);
  assert(load_ip(ip + 16) == remote_ip);
  assert(ip[9] == NET_IP_ICMP);
  const uint8_t *icmp = ip + IPV4_HEADER_LEN;
  assert(icmp[0] == ICMP_DEST_UNREACH);
  assert(icmp[1] == ICMP_CODE_PORT_UNREACH);
  const uint8_t *quoted_ip = icmp + ICMP_HEADER_LEN;
  assert(load_ip(quoted_ip + 12) == remote_ip);
  assert(load_ip(quoted_ip + 16) == local_ip);
  assert(quoted_ip[9] == NET_IP_UDP);
  const uint8_t *quoted_udp = quoted_ip + IPV4_HEADER_LEN;
  assert(load_be16(quoted_udp) == remote_port);
  assert(load_be16(quoted_udp + 2) == local_port);
}

static void test_ipv4_fragments_are_dropped_without_reassembly(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint8_t arp[ETH_HEADER_LEN + 28] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint16_t remote_port = 49153;
  uint16_t local_port = 45995;

  net_init();
  size_t arp_len = make_arp_reply(arp, remote_ip);
  net_receive_ethernet(arp, arp_len);

  size_t len = make_udp_to_closed_port(frame, remote_ip, remote_port, local_port);
  uint8_t *ip = frame + ETH_HEADER_LEN;
  write_ipv4_fragment_field(ip, IPV4_FLAG_MORE_FRAGMENTS);
  saw_frame = false;
  net_receive_ethernet(frame, len);
  assert(!saw_frame);

  len = make_udp_to_closed_port(frame, remote_ip, remote_port, local_port);
  ip = frame + ETH_HEADER_LEN;
  write_ipv4_fragment_field(ip, 1u & IPV4_FRAGMENT_OFFSET_MASK);
  saw_frame = false;
  net_receive_ethernet(frame, len);
  assert(!saw_frame);
}

static void test_broadcast_tcp_is_dropped(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + TCP_HEADER_LEN] = {0};
  uint32_t local_ip = net_ipv4(10, 0, 2, 15);
  uint32_t broadcast_ip = net_ipv4(255, 255, 255, 255);

  net_init();
  size_t len = make_tcp_to_broadcast(frame, local_ip);
  saw_tcp_segment = false;
  net_receive_ethernet(frame, len);
  assert(!saw_tcp_segment);

  len = make_tcp_to_broadcast(frame, broadcast_ip);
  saw_tcp_segment = false;
  net_receive_ethernet(frame, len);
  assert(!saw_tcp_segment);
}

static void test_broadcast_icmp_is_dropped(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + ICMP_HEADER_LEN] = {0};

  net_init();
  size_t len = make_icmp_to_broadcast(frame, ICMP_ECHO_REQUEST);
  saw_frame = false;
  saw_icmp_delivery = false;
  net_receive_ethernet(frame, len);
  assert(!saw_frame);
  assert(!saw_icmp_delivery);

  len = make_icmp_to_broadcast(frame, ICMP_ECHO_REPLY);
  saw_frame = false;
  saw_icmp_delivery = false;
  net_receive_ethernet(frame, len);
  assert(!saw_frame);
  assert(!saw_icmp_delivery);
}

static void test_invalid_tcp_flag_combinations_are_dropped(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + TCP_HEADER_LEN] = {0};
  const uint8_t invalid_flags[] = {
      TCP_FLAG_SYN | TCP_FLAG_FIN,
      TCP_FLAG_SYN | TCP_FLAG_RST,
      TCP_FLAG_FIN | TCP_FLAG_RST,
  };

  net_init();
  size_t len = make_tcp_to_local(frame, TCP_FLAG_SYN);
  saw_tcp_segment = false;
  net_receive_ethernet(frame, len);
  assert(saw_tcp_segment);

  for (size_t i = 0; i < sizeof(invalid_flags) / sizeof(invalid_flags[0]); ++i) {
    net_init();
    len = make_tcp_to_local(frame, invalid_flags[i]);
    saw_tcp_segment = false;
    net_receive_ethernet(frame, len);
    assert(!saw_tcp_segment);
  }
}

static void test_zero_tcp_ports_are_dropped(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + TCP_HEADER_LEN] = {0};

  net_init();
  size_t len = make_tcp_to_local(frame, TCP_FLAG_SYN);
  rewrite_tcp_ports(frame, 0, 80);
  saw_tcp_segment = false;
  net_receive_ethernet(frame, len);
  assert(!saw_tcp_segment);

  len = make_tcp_to_local(frame, TCP_FLAG_SYN);
  rewrite_tcp_ports(frame, 49153, 0);
  saw_tcp_segment = false;
  net_receive_ethernet(frame, len);
  assert(!saw_tcp_segment);
}

static void test_invalid_ipv4_sources_are_dropped(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint16_t remote_port = 49153;
  uint16_t local_port = 45995;
  const uint32_t invalid_sources[] = {
      net_ipv4(0, 0, 0, 0),
      net_ipv4(10, 0, 2, 15),
      net_ipv4(127, 0, 0, 1),
      net_ipv4(224, 0, 0, 1),
      net_ipv4(240, 0, 0, 1),
      net_ipv4(255, 255, 255, 255),
  };

  for (size_t i = 0; i < sizeof(invalid_sources) / sizeof(invalid_sources[0]); ++i) {
    net_init();
    size_t len = make_udp_to_closed_port(frame, net_ipv4(10, 0, 2, 2), remote_port, local_port);
    rewrite_ipv4_source(frame + ETH_HEADER_LEN, invalid_sources[i]);
    saw_frame = false;
    net_receive_ethernet(frame, len);
    assert(!saw_frame);
  }
}

static void test_ipv4_zero_ttl_is_dropped(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint16_t remote_port = 49153;
  uint16_t local_port = 45995;

  net_init();
  size_t len = make_udp_to_closed_port(frame, remote_ip, remote_port, local_port);
  rewrite_ipv4_ttl(frame + ETH_HEADER_LEN, 0);
  saw_frame = false;
  net_receive_ethernet(frame, len);

  assert(!saw_frame);
}

static void test_broadcast_udp_closed_port_does_not_emit_icmp(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint16_t remote_port = 49153;
  uint16_t local_port = 45995;

  net_init();
  size_t len = make_udp_to_closed_port(frame, remote_ip, remote_port, local_port);
  rewrite_ipv4_dest(frame + ETH_HEADER_LEN, net_ipv4(255, 255, 255, 255));
  saw_frame = false;
  net_receive_ethernet(frame, len);

  assert(!saw_frame);
}

static void test_bad_udp_checksum_is_dropped(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint16_t remote_port = 49153;
  uint16_t local_port = 45995;

  net_init();
  size_t len = make_udp_to_closed_port(frame, remote_ip, remote_port, local_port);
  write_udp_checksum(frame + ETH_HEADER_LEN + IPV4_HEADER_LEN, 0x1234);
  saw_frame = false;
  net_receive_ethernet(frame, len);

  assert(!saw_frame);
}

static void test_zero_udp_destination_port_is_dropped(void) {
  uint8_t frame[ETH_HEADER_LEN + IPV4_HEADER_LEN + UDP_HEADER_LEN] = {0};
  uint32_t remote_ip = net_ipv4(10, 0, 2, 2);
  uint16_t remote_port = 49153;
  uint16_t local_port = 45995;

  net_init();
  size_t len = make_udp_to_closed_port(frame, remote_ip, remote_port, local_port);
  rewrite_udp_ports(frame, remote_port, 0);
  saw_frame = false;
  net_receive_ethernet(frame, len);

  assert(!saw_frame);
}

static void test_arp_miss_queues_ipv4_until_reply(void) {
  uint8_t arp[ETH_HEADER_LEN + 28] = {0};
  uint32_t remote_ip = net_ipv4(93, 184, 216, 34);
  uint32_t gateway_ip = net_ipv4(10, 0, 2, 2);
  const char payload[] = "queued";

  net_init();
  saw_frame = false;
  bool queued = net_udp_send(49152, remote_ip, 5555, payload, sizeof(payload) - 1);

  assert(queued);
  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_ARP);
  const uint8_t *request = sent_frame + ETH_HEADER_LEN;
  assert(load_be16(request + 6) == ARP_REQUEST);
  assert(load_ip(request + 24) == gateway_ip);

  saw_frame = false;
  size_t arp_len = make_arp_reply(arp, gateway_ip);
  net_receive_ethernet(arp, arp_len);

  assert(saw_frame);
  assert(sent_frame[0] == 0x52);
  assert(sent_frame[1] == 0x55);
  assert(sent_frame[2] == 0x00);
  assert(sent_frame[3] == 0xaa);
  assert(sent_frame[4] == 0xbb);
  assert(sent_frame[5] == 0xcc);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_IPV4);
  const uint8_t *ip = sent_frame + ETH_HEADER_LEN;
  assert(load_ip(ip + 16) == remote_ip);
  assert(ip[9] == NET_IP_UDP);
  const uint8_t *udp = ip + IPV4_HEADER_LEN;
  assert(load_be16(udp + 2) == 5555);
}

static void test_local_subnet_routes_directly(void) {
  uint8_t arp[ETH_HEADER_LEN + 28] = {0};
  uint32_t peer_ip = net_ipv4(10, 0, 2, 44);
  const char payload[] = "local";

  net_init();
  saw_frame = false;
  bool queued = net_udp_send(49152, peer_ip, 5556, payload, sizeof(payload) - 1);

  assert(queued);
  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_ARP);
  const uint8_t *request = sent_frame + ETH_HEADER_LEN;
  assert(load_be16(request + 6) == ARP_REQUEST);
  assert(load_ip(request + 24) == peer_ip);

  saw_frame = false;
  size_t arp_len = make_arp_reply(arp, peer_ip);
  net_receive_ethernet(arp, arp_len);

  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_IPV4);
  const uint8_t *ip = sent_frame + ETH_HEADER_LEN;
  assert(load_ip(ip + 16) == peer_ip);
  assert(ip[9] == NET_IP_UDP);
  const uint8_t *udp = ip + IPV4_HEADER_LEN;
  assert(load_be16(udp + 2) == 5556);
}

static void test_ipv4_ttl_and_tos_are_preserved_on_udp_and_tcp_send(void) {
  uint8_t arp[ETH_HEADER_LEN + 28] = {0};
  uint32_t peer_ip = net_ipv4(10, 0, 2, 44);
  const char payload[] = "tos";

  net_init();
  saw_frame = false;
  bool queued = net_udp_send_ttl_tos(49152, peer_ip, 5556, 37, 0xb8, payload, sizeof(payload) - 1);
  assert(queued);
  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_ARP);

  saw_frame = false;
  size_t arp_len = make_arp_reply(arp, peer_ip);
  net_receive_ethernet(arp, arp_len);
  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_IPV4);
  const uint8_t *ip = sent_frame + ETH_HEADER_LEN;
  assert(ip[8] == 37);
  assert(ip[1] == 0xb8);
  assert(ip[9] == NET_IP_UDP);

  saw_frame = false;
  bool sent = net_tcp_send_segment_ttl_tos(49153, peer_ip, 80, 42, 0x28, 1, 0, 4096, TCP_FLAG_SYN, NULL, 0);
  assert(sent);
  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_IPV4);
  ip = sent_frame + ETH_HEADER_LEN;
  assert(ip[8] == 42);
  assert(ip[1] == 0x28);
  assert(ip[9] == NET_IP_TCP);
}

static void test_arp_learns_only_packets_targeted_at_us(void) {
  uint8_t arp[ETH_HEADER_LEN + 28] = {0};
  uint32_t remote_ip = net_ipv4(93, 184, 216, 34);
  uint32_t gateway_ip = net_ipv4(10, 0, 2, 2);
  const char payload[] = "queued";

  net_init();
  saw_frame = false;
  bool queued = net_udp_send(49152, remote_ip, 5555, payload, sizeof(payload) - 1);
  assert(queued);
  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_ARP);

  saw_frame = false;
  size_t arp_len = make_arp_reply(arp, gateway_ip);
  rewrite_arp_target_ip(arp, net_ipv4(10, 0, 2, 44));
  net_receive_ethernet(arp, arp_len);
  assert(!saw_frame);

  saw_frame = false;
  arp_len = make_arp_reply(arp, gateway_ip);
  net_receive_ethernet(arp, arp_len);
  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_IPV4);
}

static void test_arp_ignores_invalid_sender_sources(void) {
  uint8_t arp[ETH_HEADER_LEN + 28] = {0};
  uint32_t remote_ip = net_ipv4(93, 184, 216, 34);
  uint32_t gateway_ip = net_ipv4(10, 0, 2, 2);
  const char payload[] = "queued";

  net_init();
  saw_frame = false;
  bool queued = net_udp_send(49152, remote_ip, 5555, payload, sizeof(payload) - 1);
  assert(queued);
  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_ARP);

  saw_frame = false;
  size_t arp_len = make_arp_reply(arp, gateway_ip);
  rewrite_arp_sender_ip(arp, net_ipv4(0, 0, 0, 0));
  net_receive_ethernet(arp, arp_len);
  assert(!saw_frame);

  saw_frame = false;
  arp_len = make_arp_reply(arp, gateway_ip);
  net_receive_ethernet(arp, arp_len);
  assert(saw_frame);
  assert(load_be16(sent_frame + 12) == ETH_TYPE_IPV4);
}

int main(void) {
  test_icmp_port_unreachable_maps_to_udp_socket_error();
  test_icmp_port_unreachable_maps_to_tcp_socket_error();
  test_icmp_error_requires_quoted_local_source();
  test_icmp_error_requires_valid_quoted_ip_checksum();
  test_udp_closed_port_sends_icmp_port_unreachable();
  test_ipv4_fragments_are_dropped_without_reassembly();
  test_broadcast_tcp_is_dropped();
  test_broadcast_icmp_is_dropped();
  test_invalid_tcp_flag_combinations_are_dropped();
  test_zero_tcp_ports_are_dropped();
  test_invalid_ipv4_sources_are_dropped();
  test_ipv4_zero_ttl_is_dropped();
  test_broadcast_udp_closed_port_does_not_emit_icmp();
  test_bad_udp_checksum_is_dropped();
  test_zero_udp_destination_port_is_dropped();
  test_arp_miss_queues_ipv4_until_reply();
  test_local_subnet_routes_directly();
  test_ipv4_ttl_and_tos_are_preserved_on_udp_and_tcp_send();
  test_arp_learns_only_packets_targeted_at_us();
  test_arp_ignores_invalid_sender_sources();
  return 0;
}
