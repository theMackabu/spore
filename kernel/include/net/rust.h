#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct net_tcp_options {
  uint16_t mss;
  uint8_t window_scale;
  uint8_t has_mss;
};

bool spore_net_validate_ethernet(const uint8_t *frame, size_t len);
bool spore_net_decode_tcp_options(const uint8_t *options, size_t len, struct net_tcp_options *out);
