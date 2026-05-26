#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  NET_IP_ICMP = 1,
  NET_IP_TCP = 6,
  NET_IP_UDP = 17,
};

struct net_config {
  uint32_t local_ip;
  uint32_t gateway_ip;
  uint32_t netmask;
  uint32_t dns_ip;
  uint8_t configured;
};

uint32_t net_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void net_init(void);
void net_poll(void);
void net_receive_ethernet(const uint8_t *frame, size_t len);
bool net_udp_send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void *payload, size_t len);
bool net_tcp_send_segment(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, uint32_t seq, uint32_t ack,
                          uint8_t flags, const void *payload, size_t len);
bool net_icmp_send_echo(uint32_t dst_ip, const void *payload, size_t len);
void net_get_config(struct net_config *out);
void net_set_config(const struct net_config *cfg);
