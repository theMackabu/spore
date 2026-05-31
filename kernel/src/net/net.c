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
  ICMP_DEST_UNREACH = 3,
  ICMP_ECHO_REQUEST = 8,
  ICMP_CODE_PORT_UNREACH = 3,
  IPV4_HEADER_LEN = 20,
  UDP_HEADER_LEN = 8,
  TCP_HEADER_LEN = 20,
  ICMP_HEADER_LEN = 8,
  ETH_HEADER_LEN = 14,
  MAX_FRAME = 1514,
  ARP_CACHE_ENTRIES = 16,
  ARP_PENDING_ENTRIES = 16,
  ARP_RETRY_TICKS = 100,
  NET_ECONNREFUSED = 111,
};

static const uint8_t local_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

struct arp_cache_entry {
  bool valid;
  uint32_t ip;
  uint8_t mac[6];
  uint64_t age;
};

struct arp_pending_packet {
  bool used;
  uint32_t next_hop;
  uint8_t frame[MAX_FRAME];
  uint32_t len;
  uint64_t age;
  uint64_t next_retry_tick;
};

static uint32_t local_ip;
static uint32_t gateway_ip;
static uint32_t netmask;
static uint32_t dns_ip;
static bool net_configured;
static struct arp_cache_entry arp_cache[ARP_CACHE_ENTRIES];
static struct arp_pending_packet arp_pending[ARP_PENDING_ENTRIES];
static uint8_t gateway_mac[6];
static bool gateway_mac_valid;
static uint64_t arp_cache_clock;
static uint64_t arp_pending_clock;
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

static uint16_t ip_payload_checksum(uint8_t proto, uint32_t src_ip, uint32_t dst_ip, const void *payload, size_t len) {
  uint8_t pseudo[12 + 1480];
  if (len > sizeof(pseudo) - 12) { return 0; }
  store_ip(pseudo, src_ip);
  store_ip(pseudo + 4, dst_ip);
  pseudo[8] = 0;
  pseudo[9] = proto;
  store_be16(pseudo + 10, (uint16_t)len);
  kmemcpy(pseudo + 12, payload, len);
  return checksum(pseudo, 12 + len);
}

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *tcp, size_t len) {
  return ip_payload_checksum(NET_IP_TCP, src_ip, dst_ip, tcp, len);
}

static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *udp, size_t len) {
  return ip_payload_checksum(NET_IP_UDP, src_ip, dst_ip, udp, len);
}

static bool mac_is_for_us(const uint8_t *dst) {
  return kmemcmp(dst, local_mac, sizeof(local_mac)) == 0 || kmemcmp(dst, broadcast_mac, sizeof(broadcast_mac)) == 0;
}

static uint32_t route_next_hop(uint32_t dst_ip) {
  if (dst_ip == gateway_ip || dst_ip == 0xffffffffu || (netmask != 0 && dst_ip == (local_ip | ~netmask))) {
    return dst_ip;
  }
  if (netmask != 0 && (dst_ip & netmask) == (local_ip & netmask)) { return dst_ip; }
  return gateway_ip == 0 ? dst_ip : gateway_ip;
}

static bool is_loopback(uint32_t ip) {
  return (ip & 0xffu) == 127u;
}

bool net_is_loopback_addr(uint32_t ip) {
  return is_loopback(ip);
}

static bool is_broadcast(uint32_t ip) {
  return ip == 0xffffffffu || (netmask != 0 && ip == (local_ip | ~netmask));
}

bool net_is_broadcast_addr(uint32_t ip) {
  return is_broadcast(ip);
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

static void arp_cache_put(uint32_t ip, const uint8_t mac[6]) {
  if (ip == 0) { return; }
  struct arp_cache_entry *slot = &arp_cache[0];
  for (size_t i = 0; i < ARP_CACHE_ENTRIES; ++i) {
    if (arp_cache[i].valid && arp_cache[i].ip == ip) {
      slot = &arp_cache[i];
      break;
    }
    if (!arp_cache[i].valid) {
      slot = &arp_cache[i];
      break;
    }
    if (arp_cache[i].age < slot->age) { slot = &arp_cache[i]; }
  }
  slot->valid = true;
  slot->ip = ip;
  kmemcpy(slot->mac, mac, 6);
  slot->age = ++arp_cache_clock;
}

static bool arp_cache_get(uint32_t ip, uint8_t out[6]) {
  for (size_t i = 0; i < ARP_CACHE_ENTRIES; ++i) {
    if (!arp_cache[i].valid || arp_cache[i].ip != ip) { continue; }
    arp_cache[i].age = ++arp_cache_clock;
    kmemcpy(out, arp_cache[i].mac, 6);
    return true;
  }
  return false;
}

static bool resolve_mac_cached(uint32_t dst_ip, uint8_t out[6], uint32_t *next_hop_out) {
  uint32_t next_hop = route_next_hop(dst_ip);
  if (next_hop_out != NULL) { *next_hop_out = next_hop; }
  if (is_broadcast(dst_ip) || is_broadcast(next_hop)) {
    kmemcpy(out, broadcast_mac, sizeof(broadcast_mac));
    return true;
  }
  if (arp_cache_get(next_hop, out)) { return true; }
  if (gateway_mac_valid && next_hop == gateway_ip) {
    kmemcpy(out, gateway_mac, 6);
    return true;
  }
  return false;
}

static void arp_request_now(uint32_t next_hop) {
  if (next_hop == 0 || is_broadcast(next_hop)) { return; }
  uint8_t zero_mac[6] = {0};
  send_arp(ARP_REQUEST, broadcast_mac, next_hop, zero_mac);
}

static bool arp_pending_has_next_hop(uint32_t next_hop) {
  for (size_t i = 0; i < ARP_PENDING_ENTRIES; ++i) {
    if (arp_pending[i].used && arp_pending[i].next_hop == next_hop) { return true; }
  }
  return false;
}

static struct arp_pending_packet *arp_pending_slot(void) {
  struct arp_pending_packet *oldest = &arp_pending[0];
  for (size_t i = 0; i < ARP_PENDING_ENTRIES; ++i) {
    if (!arp_pending[i].used) { return &arp_pending[i]; }
    if (arp_pending[i].age < oldest->age) { oldest = &arp_pending[i]; }
  }
  return oldest;
}

static bool queue_ipv4_for_arp(uint32_t next_hop, const uint8_t *frame, uint32_t len) {
  if (next_hop == 0 || frame == NULL || len > MAX_FRAME) { return false; }
  bool request_needed = !arp_pending_has_next_hop(next_hop);
  struct arp_pending_packet *slot = arp_pending_slot();
  slot->used = true;
  slot->next_hop = next_hop;
  slot->len = len;
  slot->age = ++arp_pending_clock;
  slot->next_retry_tick = cell_uptime_ticks() + ARP_RETRY_TICKS;
  kmemcpy(slot->frame, frame, len);
  if (request_needed) { arp_request_now(next_hop); }
  return true;
}

static void arp_flush_pending(uint32_t next_hop, const uint8_t mac[6]) {
  if (mac == NULL) { return; }
  for (size_t i = 0; i < ARP_PENDING_ENTRIES; ++i) {
    if (!arp_pending[i].used || arp_pending[i].next_hop != next_hop) { continue; }
    kmemcpy(arp_pending[i].frame, mac, 6);
    (void)virtio_net_send_frame(arp_pending[i].frame, arp_pending[i].len);
    arp_pending[i] = (struct arp_pending_packet){0};
  }
}

static void arp_retry_pending(void) {
  uint64_t now = cell_uptime_ticks();
  for (size_t i = 0; i < ARP_PENDING_ENTRIES; ++i) {
    if (!arp_pending[i].used || now < arp_pending[i].next_retry_tick) { continue; }
    uint8_t mac[6];
    if (arp_cache_get(arp_pending[i].next_hop, mac)) {
      arp_flush_pending(arp_pending[i].next_hop, mac);
      continue;
    }
    arp_request_now(arp_pending[i].next_hop);
    arp_pending[i].next_retry_tick = now + ARP_RETRY_TICKS;
  }
}

static uint32_t build_ipv4_frame(uint8_t *frame, const uint8_t dst_mac[6], uint8_t proto, uint32_t dst_ip,
                                 const void *payload, size_t len) {
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
  return (uint32_t)(ETH_HEADER_LEN + IPV4_HEADER_LEN + len);
}

static bool send_ipv4(uint8_t proto, uint32_t dst_ip, const void *payload, size_t len) {
  if (len + ETH_HEADER_LEN + IPV4_HEADER_LEN > MAX_FRAME) { return false; }
  uint8_t dst_mac[6];
  uint32_t next_hop = 0;
  uint8_t frame[MAX_FRAME];
  uint32_t frame_len = 0;
  if (!resolve_mac_cached(dst_ip, dst_mac, &next_hop)) {
    uint8_t placeholder_mac[6] = {0};
    frame_len = build_ipv4_frame(frame, placeholder_mac, proto, dst_ip, payload, len);
    return queue_ipv4_for_arp(next_hop, frame, frame_len);
  }

  frame_len = build_ipv4_frame(frame, dst_mac, proto, dst_ip, payload, len);
  return virtio_net_send_frame(frame, frame_len);
}

static void send_icmp_port_unreachable(uint32_t dst_ip, const uint8_t *original_ip, size_t original_ihl,
                                       const uint8_t *original_payload, size_t original_payload_len) {
  size_t quoted_payload_len = original_payload_len < 8 ? original_payload_len : 8;
  size_t quoted_len = original_ihl + quoted_payload_len;
  if (ICMP_HEADER_LEN + quoted_len > 1472) { return; }

  uint8_t packet[ICMP_HEADER_LEN + 1472];
  packet[0] = ICMP_DEST_UNREACH;
  packet[1] = ICMP_CODE_PORT_UNREACH;
  store_be16(packet + 2, 0);
  store_be16(packet + 4, 0);
  store_be16(packet + 6, 0);
  kmemcpy(packet + ICMP_HEADER_LEN, original_ip, original_ihl);
  if (quoted_payload_len != 0) { kmemcpy(packet + ICMP_HEADER_LEN + original_ihl, original_payload, quoted_payload_len); }
  store_be16(packet + 2, checksum(packet, ICMP_HEADER_LEN + quoted_len));
  (void)send_ipv4(NET_IP_ICMP, dst_ip, packet, ICMP_HEADER_LEN + quoted_len);
}

bool net_udp_send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void *payload, size_t len) {
  if (is_loopback(dst_ip)) {
    return cell_net_deliver_udp(dst_ip, src_port, dst_port, payload, len);
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
  uint16_t sum = udp_checksum(local_ip, dst_ip, packet, UDP_HEADER_LEN + len);
  store_be16(packet + 6, sum == 0 ? 0xffffu : sum);
  return send_ipv4(NET_IP_UDP, dst_ip, packet, UDP_HEADER_LEN + len);
}

bool net_tcp_send_segment(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint16_t window, uint8_t flags, const void *payload, size_t len) {
  return net_tcp_send_segment_options(src_port, dst_ip, dst_port, seq, ack, window, flags, payload, len, NULL, 0);
}

bool net_tcp_send_segment_options(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, uint32_t seq, uint32_t ack,
                                  uint16_t window, uint8_t flags, const void *payload, size_t len,
                                  const void *options, size_t options_len) {
  if (is_loopback(dst_ip)) {
    cell_net_deliver_tcp(dst_ip, src_port, dst_port, seq, ack, window, flags, options, options_len, payload, len);
    return true;
  }
  if (!cell_egress_allowed(NET_IP_TCP, dst_ip, dst_port)) {
    kprintf("[spore] net: tx denied proto=tcp dst=%x:%u\n", (unsigned)dst_ip, (unsigned)dst_port);
    return false;
  }
  if ((options_len & 3u) != 0 || options_len > 40 || len + TCP_HEADER_LEN + options_len > 1460) { return false; }
  uint8_t packet[TCP_HEADER_LEN + 40 + 1460];
  size_t header_len = TCP_HEADER_LEN + options_len;
  store_be16(packet, src_port);
  store_be16(packet + 2, dst_port);
  store_be32(packet + 4, seq);
  store_be32(packet + 8, ack);
  packet[12] = (uint8_t)((header_len / 4u) << 4);
  packet[13] = flags;
  store_be16(packet + 14, window);
  store_be16(packet + 16, 0);
  store_be16(packet + 18, 0);
  if (options_len != 0) { kmemcpy(packet + TCP_HEADER_LEN, options, options_len); }
  if (len != 0) { kmemcpy(packet + header_len, payload, len); }
  store_be16(packet + 16, tcp_checksum(local_ip, dst_ip, packet, header_len + len));
  return send_ipv4(NET_IP_TCP, dst_ip, packet, header_len + len);
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
  arp_cache_put(sender_ip, sender_mac);
  if (sender_ip == gateway_ip) {
    kmemcpy(gateway_mac, sender_mac, sizeof(gateway_mac));
    gateway_mac_valid = true;
  }
  arp_flush_pending(sender_ip, sender_mac);
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
  } else if (icmp[0] == ICMP_DEST_UNREACH && icmp[1] == ICMP_CODE_PORT_UNREACH) {
    const uint8_t *quoted_ip = icmp + ICMP_HEADER_LEN;
    size_t quoted_len = len - ICMP_HEADER_LEN;
    if (quoted_len < IPV4_HEADER_LEN || (quoted_ip[0] >> 4) != 4) { return; }
    size_t quoted_ihl = (size_t)(quoted_ip[0] & 0xf) * 4;
    if (quoted_ihl < IPV4_HEADER_LEN || quoted_len < quoted_ihl + UDP_HEADER_LEN) { return; }
    uint32_t original_dst_ip = load_ip(quoted_ip + 16);
    const uint8_t *quoted_l4 = quoted_ip + quoted_ihl;
    uint16_t original_src_port = load_be16(quoted_l4);
    uint16_t original_dst_port = load_be16(quoted_l4 + 2);
    if (quoted_ip[9] == NET_IP_UDP) {
      (void)cell_net_deliver_udp_error(original_dst_ip, original_dst_port, original_src_port, NET_ECONNREFUSED);
    } else if (quoted_ip[9] == NET_IP_TCP) {
      (void)cell_net_deliver_tcp_error(original_dst_ip, original_dst_port, original_src_port, NET_ECONNREFUSED);
    }
  }
}

static void handle_udp(uint32_t src_ip, uint32_t dst_ip, const uint8_t *original_ip, size_t original_ihl,
                       const uint8_t *udp, size_t len) {
  if (len < UDP_HEADER_LEN) { return; }
  uint16_t src_port = load_be16(udp);
  uint16_t dst_port = load_be16(udp + 2);
  uint16_t udp_len = load_be16(udp + 4);
  if (udp_len < UDP_HEADER_LEN || udp_len > len) { return; }
  uint16_t sum = load_be16(udp + 6);
  if (sum != 0 && udp_checksum(src_ip, dst_ip, udp, udp_len) != 0) { return; }
  if (!cell_net_deliver_udp(src_ip, src_port, dst_port, udp + UDP_HEADER_LEN, udp_len - UDP_HEADER_LEN) &&
      dst_ip == local_ip) {
    send_icmp_port_unreachable(src_ip, original_ip, original_ihl, udp, udp_len);
  }
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
  uint16_t window = load_be16(tcp + 14);
  uint8_t flags = tcp[13];
  cell_net_deliver_tcp(src_ip, src_port, dst_port, seq, ack, window, flags, tcp + TCP_HEADER_LEN,
                       offset - TCP_HEADER_LEN, tcp + offset, len - offset);
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
    handle_udp(src_ip, dst_ip, ip, ihl, payload, payload_len);
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
  arp_retry_pending();
}

void net_init(void) {
  local_ip = net_ipv4(10, 0, 2, 15);
  gateway_ip = net_ipv4(10, 0, 2, 2);
  netmask = net_ipv4(255, 255, 255, 0);
  dns_ip = net_ipv4(10, 0, 2, 3);
  net_configured = false;
  gateway_mac_valid = false;
  arp_cache_clock = 0;
  arp_pending_clock = 0;
  for (size_t i = 0; i < ARP_CACHE_ENTRIES; ++i) {
    arp_cache[i].valid = false;
  }
  for (size_t i = 0; i < ARP_PENDING_ENTRIES; ++i) {
    arp_pending[i] = (struct arp_pending_packet){0};
  }
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
  arp_pending_clock = 0;
  for (size_t i = 0; i < ARP_CACHE_ENTRIES; ++i) {
    arp_cache[i].valid = false;
  }
  for (size_t i = 0; i < ARP_PENDING_ENTRIES; ++i) {
    arp_pending[i] = (struct arp_pending_packet){0};
  }
  kprintf("[spore] net: ipv4 %u.%u.%u.%u gateway %u.%u.%u.%u dns %u.%u.%u.%u\n", (unsigned)(local_ip & 0xffu),
          (unsigned)((local_ip >> 8) & 0xffu), (unsigned)((local_ip >> 16) & 0xffu),
          (unsigned)((local_ip >> 24) & 0xffu), (unsigned)(gateway_ip & 0xffu), (unsigned)((gateway_ip >> 8) & 0xffu),
          (unsigned)((gateway_ip >> 16) & 0xffu), (unsigned)((gateway_ip >> 24) & 0xffu), (unsigned)(dns_ip & 0xffu),
          (unsigned)((dns_ip >> 8) & 0xffu), (unsigned)((dns_ip >> 16) & 0xffu), (unsigned)((dns_ip >> 24) & 0xffu));
}
