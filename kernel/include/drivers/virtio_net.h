#pragma once

#include <stdbool.h>
#include <stdint.h>

struct virtio_net_stats {
  uint64_t rx_bytes;
  uint64_t rx_packets;
  uint64_t tx_bytes;
  uint64_t tx_packets;
};

bool virtio_net_init(uint64_t hhdm_offset);
bool virtio_net_smoke_tx(void);
bool virtio_net_send_frame(const void *frame, uint32_t len);
void virtio_net_poll(void);
struct virtio_net_stats virtio_net_stats(void);
