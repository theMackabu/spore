#include "internal.h"
#include "net/net.h"
#include "net/packet.h"

constexpr uint8_t RECEIVE_WINDOW_SCALE = 2;

static uint16_t advertised_window(const struct open_file *file, uint8_t scale) {
  uint32_t limit = socket_effective_buffer(file, true);
  uint32_t used = tcp_rx_available(file);
  uint32_t room = used >= limit ? 0 : limit - used;
  room >>= scale;
  return room > UINT16_MAX ? UINT16_MAX : (uint16_t)room;
}

uint16_t tcp_window(const struct open_file *file) {
  return advertised_window(file, file->tcp_local_window_scale);
}

void tcp_apply_syn_options(struct open_file *file, uint16_t window, const void *options, size_t len) {
  struct net_tcp_options decoded = spore_net_decode_tcp_options(options, len);
  file->tcp_remote_window = window;
  file->tcp_remote_window_scale = decoded.window_scale;
  file->tcp_local_window_scale = decoded.has_window_scale ? RECEIVE_WINDOW_SCALE : 0;
  file->tcp_remote_mss = decoded.mss;
}

bool tcp_send_syn(const struct open_file *file, uint32_t seq, bool reply) {
  static const uint8_t options[] = {2, 4, CELL_TCP_TX_CAP >> 8, CELL_TCP_TX_CAP & 255, 1, 3, 3, RECEIVE_WINDOW_SCALE};
  size_t len = !reply || file->tcp_local_window_scale ? sizeof(options) : 4;
  return net_tcp_send_segment_options_ttl_tos(file->net_ns, file->tcp_local_port, file->tcp_remote_ip, file->tcp_remote_port,
                                              socket_effective_ttl(file), (uint8_t)file->ip_tos, seq,
                                              reply ? file->tcp_ack : 0, advertised_window(file, 0),
                                              reply ? TCP_SYN | TCP_ACK : TCP_SYN, NULL, 0, options, len);
}
