#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct net_tcp_options {
  uint16_t mss;
  uint8_t window_scale;
};

struct net_rx_header {
  uint16_t eth_type;
  uint8_t ip_protocol;
  uint8_t ttl;
  uint16_t ip_header_len;
  uint16_t transport_len;
  uint32_t src_ip;
  uint32_t dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
  uint32_t sequence;
  uint32_t acknowledgement;
  uint16_t window;
  uint8_t tcp_flags;
  uint8_t tcp_options_len;
  uint16_t arp_operation;
  uint8_t icmp_type;
  uint8_t quoted_protocol;
  uint32_t quoted_src;
  uint32_t quoted_dst;
};

bool spore_net_decode_ethernet(const uint8_t *frame, size_t len, struct net_rx_header *out);
struct net_tcp_options spore_net_decode_tcp_options(const uint8_t *options, size_t len);
