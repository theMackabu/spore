#include "net.h"

#include "cell.h"
#include "kprintf.h"
#include "mem.h"
#include "virtio_net.h"

enum {
  ETH_TYPE_ARP = 0x0806,
  ETH_TYPE_IPV4 = 0x0800,
  ARP_REQUEST = 1,
  ARP_REPLY = 2,
  ICMP_ECHO_REPLY = 0,
  ICMP_ECHO_REQUEST = 8,
  IPV4_HEADER_LEN = 20,
  UDP_HEADER_LEN = 8,
  TCP_HEADER_LEN = 20,
  ICMP_HEADER_LEN = 8,
  ETH_HEADER_LEN = 14,
  MAX_FRAME = 1514,
};

static const uint8_t local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static uint32_t local_ip;
static uint32_t gateway_ip;
static uint32_t netmask;
static uint32_t dns_ip;
static bool net_configured;
static uint8_t gateway_mac[6];
static bool gateway_mac_valid;
static uint16_t next_ip_id = 1;

uint32_t net_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

static uint16_t load_be16(const uint8_t *p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void store_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static uint32_t load_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void store_ip(uint8_t *p, uint32_t ip) {
  p[0] = (uint8_t)ip;
  p[1] = (uint8_t)(ip >> 8);
  p[2] = (uint8_t)(ip >> 16);
  p[3] = (uint8_t)(ip >> 24);
}

static uint32_t load_ip(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t checksum(const void *data, size_t len) {
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

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *tcp, size_t len) {
  uint8_t pseudo[12 + TCP_HEADER_LEN + 1460];
  if (len > TCP_HEADER_LEN + 1460) { return 0; }
  store_ip(pseudo, src_ip);
  store_ip(pseudo + 4, dst_ip);
  pseudo[8] = 0;
  pseudo[9] = NET_IP_TCP;
  store_be16(pseudo + 10, (uint16_t)len);
  kmemcpy(pseudo + 12, tcp, len);
  return checksum(pseudo, 12 + len);
}

static bool mac_is_for_us(const uint8_t *dst) {
  return kmemcmp(dst, local_mac, sizeof(local_mac)) == 0 || kmemcmp(dst, broadcast_mac, sizeof(broadcast_mac)) == 0;
}

static uint32_t route_next_hop(uint32_t dst_ip) {
  if (dst_ip == gateway_ip || dst_ip == 0xffffffffu || (netmask != 0 && dst_ip == (local_ip | ~netmask))) {
    return dst_ip;
  }
  return gateway_ip;
}

static bool is_loopback(uint32_t ip) {
  return (ip & 0xffu) == 127u;
}

static bool is_broadcast(uint32_t ip) {
  return ip == 0xffffffffu || (netmask != 0 && ip == (local_ip | ~netmask));
}

static void send_arp(uint16_t op, const uint8_t *dst_mac, uint32_t target_ip, const uint8_t *target_mac) {
  uint8_t frame[ETH_HEADER_LEN + 28];
  kmemcpy(frame, dst_mac, 6);
  kmemcpy(frame + 6, local_mac, 6);
  store_be16(frame + 12, ETH_TYPE_ARP);
  uint8_t *arp = frame + ETH_HEADER_LEN;
  store_be16(arp, 1);
  store_be16(arp + 2, ETH_TYPE_IPV4);
  arp[4] = 6;
  arp[5] = 4;
  store_be16(arp + 6, op);
  kmemcpy(arp + 8, local_mac, 6);
  store_ip(arp + 14, local_ip);
  kmemcpy(arp + 18, target_mac, 6);
  store_ip(arp + 24, target_ip);
  (void)virtio_net_send_frame(frame, sizeof(frame));
}

static bool resolve_mac(uint32_t dst_ip, uint8_t out[6]) {
  uint32_t next_hop = route_next_hop(dst_ip);
  if (is_broadcast(dst_ip) || is_broadcast(next_hop)) {
    kmemcpy(out, broadcast_mac, sizeof(broadcast_mac));
    return true;
  }
  if (gateway_mac_valid && next_hop == gateway_ip) {
    kmemcpy(out, gateway_mac, 6);
    return true;
  }

  uint8_t zero_mac[6] = {0};
  send_arp(ARP_REQUEST, broadcast_mac, next_hop, zero_mac);
  for (uint32_t spin = 0; spin < 200000; ++spin) {
    virtio_net_poll();
    if (gateway_mac_valid && next_hop == gateway_ip) {
      kmemcpy(out, gateway_mac, 6);
      return true;
    }
  }
  return false;
}

static bool send_ipv4(uint8_t proto, uint32_t dst_ip, const void *payload, size_t len) {
  if (len + ETH_HEADER_LEN + IPV4_HEADER_LEN > MAX_FRAME) { return false; }
  uint8_t dst_mac[6];
  if (!resolve_mac(dst_ip, dst_mac)) {
    kprintf("[spore] net: arp unresolved dst=%x\n", (unsigned)dst_ip);
    return false;
  }

  uint8_t frame[MAX_FRAME];
  kmemcpy(frame, dst_mac, 6);
  kmemcpy(frame + 6, local_mac, 6);
  store_be16(frame + 12, ETH_TYPE_IPV4);
  uint8_t *ip = frame + ETH_HEADER_LEN;
  ip[0] = 0x45;
  ip[1] = 0;
  store_be16(ip + 2, (uint16_t)(IPV4_HEADER_LEN + len));
  store_be16(ip + 4, next_ip_id++);
  store_be16(ip + 6, 0);
  ip[8] = 64;
  ip[9] = proto;
  store_be16(ip + 10, 0);
  store_ip(ip + 12, local_ip);
  store_ip(ip + 16, dst_ip);
  store_be16(ip + 10, checksum(ip, IPV4_HEADER_LEN));
  kmemcpy(ip + IPV4_HEADER_LEN, payload, len);
  return virtio_net_send_frame(frame, (uint32_t)(ETH_HEADER_LEN + IPV4_HEADER_LEN + len));
}

bool net_udp_send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void *payload, size_t len) {
  if (is_loopback(dst_ip)) {
    cell_net_deliver_udp(dst_ip, src_port, dst_port, payload, len);
    return true;
  }
  if (!cell_egress_allowed(NET_IP_UDP, dst_ip, dst_port)) {
    kprintf("[spore] net: tx denied proto=udp dst=%x:%u\n", (unsigned)dst_ip, (unsigned)dst_port);
    return false;
  }
  if (len + UDP_HEADER_LEN > 1472) { return false; }
  uint8_t packet[UDP_HEADER_LEN + 1472];
  store_be16(packet, src_port);
  store_be16(packet + 2, dst_port);
  store_be16(packet + 4, (uint16_t)(UDP_HEADER_LEN + len));
  store_be16(packet + 6, 0);
  kmemcpy(packet + UDP_HEADER_LEN, payload, len);
  return send_ipv4(NET_IP_UDP, dst_ip, packet, UDP_HEADER_LEN + len);
}

bool net_tcp_send_segment(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint16_t window, uint8_t flags, const void *payload, size_t len) {
  if (!cell_egress_allowed(NET_IP_TCP, dst_ip, dst_port)) {
    kprintf("[spore] net: tx denied proto=tcp dst=%x:%u\n", (unsigned)dst_ip, (unsigned)dst_port);
    return false;
  }
  if (len + TCP_HEADER_LEN > 1460) { return false; }
  uint8_t packet[TCP_HEADER_LEN + 1460];
  store_be16(packet, src_port);
  store_be16(packet + 2, dst_port);
  store_be32(packet + 4, seq);
  store_be32(packet + 8, ack);
  packet[12] = (uint8_t)(5u << 4);
  packet[13] = flags;
  store_be16(packet + 14, window);
  store_be16(packet + 16, 0);
  store_be16(packet + 18, 0);
  if (len != 0) { kmemcpy(packet + TCP_HEADER_LEN, payload, len); }
  store_be16(packet + 16, tcp_checksum(local_ip, dst_ip, packet, TCP_HEADER_LEN + len));
  return send_ipv4(NET_IP_TCP, dst_ip, packet, TCP_HEADER_LEN + len);
}

bool net_icmp_send_echo(uint32_t dst_ip, const void *payload, size_t len) {
  if (is_loopback(dst_ip)) {
    uint8_t reply[ICMP_HEADER_LEN + 1472];
    if (len + ICMP_HEADER_LEN > sizeof(reply)) { return false; }
    reply[0] = ICMP_ECHO_REPLY;
    reply[1] = 0;
    store_be16(reply + 2, 0);
    store_be16(reply + 4, 1);
    store_be16(reply + 6, 1);
    kmemcpy(reply + ICMP_HEADER_LEN, payload, len);
    store_be16(reply + 2, checksum(reply, ICMP_HEADER_LEN + len));
    cell_net_deliver_icmp(dst_ip, reply, ICMP_HEADER_LEN + len);
    return true;
  }
  if (len + ICMP_HEADER_LEN > 1472) { return false; }
  uint8_t packet[ICMP_HEADER_LEN + 1472];
  packet[0] = ICMP_ECHO_REQUEST;
  packet[1] = 0;
  store_be16(packet + 2, 0);
  store_be16(packet + 4, 1);
  store_be16(packet + 6, 1);
  kmemcpy(packet + ICMP_HEADER_LEN, payload, len);
  store_be16(packet + 2, checksum(packet, ICMP_HEADER_LEN + len));
  return send_ipv4(NET_IP_ICMP, dst_ip, packet, ICMP_HEADER_LEN + len);
}

static void handle_arp(const uint8_t *arp, size_t len) {
  if (len < 28 || load_be16(arp) != 1 || load_be16(arp + 2) != ETH_TYPE_IPV4 || arp[4] != 6 || arp[5] != 4) { return; }
  uint16_t op = load_be16(arp + 6);
  const uint8_t *sender_mac = arp + 8;
  uint32_t sender_ip = load_ip(arp + 14);
  uint32_t target_ip = load_ip(arp + 24);
  if (sender_ip == gateway_ip) {
    kmemcpy(gateway_mac, sender_mac, sizeof(gateway_mac));
    gateway_mac_valid = true;
  }
  if (op == ARP_REQUEST && target_ip == local_ip) { send_arp(ARP_REPLY, sender_mac, sender_ip, sender_mac); }
}

static void handle_icmp(uint32_t src_ip, const uint8_t *icmp, size_t len) {
  if (len < ICMP_HEADER_LEN || checksum(icmp, len) != 0) { return; }
  if (icmp[0] == ICMP_ECHO_REQUEST) {
    uint8_t reply[ICMP_HEADER_LEN + 1472];
    if (len > sizeof(reply)) { return; }
    kmemcpy(reply, icmp, len);
    reply[0] = ICMP_ECHO_REPLY;
    store_be16(reply + 2, 0);
    store_be16(reply + 2, checksum(reply, len));
    (void)send_ipv4(NET_IP_ICMP, src_ip, reply, len);
  } else if (icmp[0] == ICMP_ECHO_REPLY) {
    cell_net_deliver_icmp(src_ip, icmp, len);
  }
}

static void handle_udp(uint32_t src_ip, const uint8_t *udp, size_t len) {
  if (len < UDP_HEADER_LEN) { return; }
  uint16_t src_port = load_be16(udp);
  uint16_t dst_port = load_be16(udp + 2);
  uint16_t udp_len = load_be16(udp + 4);
  if (udp_len < UDP_HEADER_LEN || udp_len > len) { return; }
  cell_net_deliver_udp(src_ip, src_port, dst_port, udp + UDP_HEADER_LEN, udp_len - UDP_HEADER_LEN);
}

static void handle_tcp(uint32_t src_ip, const uint8_t *tcp, size_t len) {
  if (len < TCP_HEADER_LEN) { return; }
  if (tcp_checksum(src_ip, local_ip, tcp, len) != 0) { return; }
  size_t offset = (size_t)(tcp[12] >> 4) * 4;
  if (offset < TCP_HEADER_LEN || offset > len) { return; }
  uint16_t src_port = load_be16(tcp);
  uint16_t dst_port = load_be16(tcp + 2);
  uint32_t seq = load_be32(tcp + 4);
  uint32_t ack = load_be32(tcp + 8);
  uint8_t flags = tcp[13];
  cell_net_deliver_tcp(src_ip, src_port, dst_port, seq, ack, flags, tcp + offset, len - offset);
}

static void handle_ipv4(const uint8_t *ip, size_t len) {
  if (len < IPV4_HEADER_LEN || (ip[0] >> 4) != 4) { return; }
  size_t ihl = (size_t)(ip[0] & 0xf) * 4;
  if (ihl < IPV4_HEADER_LEN || ihl > len) { return; }
  uint16_t total = load_be16(ip + 2);
  if (total < ihl || total > len || checksum(ip, ihl) != 0) { return; }
  uint32_t dst_ip = load_ip(ip + 16);
  if (dst_ip != local_ip && dst_ip != 0xffffffffu && !is_broadcast(dst_ip)) { return; }
  uint32_t src_ip = load_ip(ip + 12);
  const uint8_t *payload = ip + ihl;
  size_t payload_len = total - ihl;
  if (ip[9] == NET_IP_ICMP) {
    handle_icmp(src_ip, payload, payload_len);
  } else if (ip[9] == NET_IP_TCP) {
    handle_tcp(src_ip, payload, payload_len);
  } else if (ip[9] == NET_IP_UDP) {
    handle_udp(src_ip, payload, payload_len);
  }
}

void net_receive_ethernet(const uint8_t *frame, size_t len) {
  if (len < ETH_HEADER_LEN || !mac_is_for_us(frame)) { return; }
  uint16_t type = load_be16(frame + 12);
  if (type == ETH_TYPE_ARP) {
    handle_arp(frame + ETH_HEADER_LEN, len - ETH_HEADER_LEN);
  } else if (type == ETH_TYPE_IPV4) {
    handle_ipv4(frame + ETH_HEADER_LEN, len - ETH_HEADER_LEN);
  }
}

void net_poll(void) {
  virtio_net_poll();
}

void net_init(void) {
  local_ip = net_ipv4(10, 0, 2, 15);
  gateway_ip = net_ipv4(10, 0, 2, 2);
  netmask = net_ipv4(255, 255, 255, 0);
  dns_ip = net_ipv4(10, 0, 2, 3);
  net_configured = false;
  gateway_mac_valid = false;
  kprintf("[spore] net: ipv4 fallback 10.0.2.15 gateway 10.0.2.2; waiting for network.service\n");
}

void net_get_config(struct net_config *out) {
  if (out == NULL) { return; }
  out->local_ip = local_ip;
  out->gateway_ip = gateway_ip;
  out->netmask = netmask;
  out->dns_ip = dns_ip;
  out->configured = net_configured ? 1u : 0u;
}

void net_set_config(const struct net_config *cfg) {
  if (cfg == NULL) { return; }
  local_ip = cfg->local_ip;
  gateway_ip = cfg->gateway_ip;
  netmask = cfg->netmask;
  dns_ip = cfg->dns_ip;
  net_configured = cfg->configured != 0;
  gateway_mac_valid = false;
  kprintf("[spore] net: ipv4 %u.%u.%u.%u gateway %u.%u.%u.%u dns %u.%u.%u.%u\n", (unsigned)(local_ip & 0xffu),
          (unsigned)((local_ip >> 8) & 0xffu), (unsigned)((local_ip >> 16) & 0xffu),
          (unsigned)((local_ip >> 24) & 0xffu), (unsigned)(gateway_ip & 0xffu), (unsigned)((gateway_ip >> 8) & 0xffu),
          (unsigned)((gateway_ip >> 16) & 0xffu), (unsigned)((gateway_ip >> 24) & 0xffu), (unsigned)(dns_ip & 0xffu),
          (unsigned)((dns_ip >> 8) & 0xffu), (unsigned)((dns_ip >> 16) & 0xffu), (unsigned)((dns_ip >> 24) & 0xffu));
}
