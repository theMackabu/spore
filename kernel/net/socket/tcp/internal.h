#pragma once

#include "../internal.h"

enum {
  TCP_FIN = 0x01,
  TCP_SYN = 0x02,
  TCP_RST = 0x04,
  TCP_PSH = 0x08,
  TCP_ACK = 0x10,
};

bool tcp_send_syn(const struct open_file *file, uint32_t seq, bool reply);
void tcp_apply_syn_options(struct open_file *file, uint16_t window, const void *options, size_t len);
void tcp_close_backlog(struct open_file *listener);
void tcp_ready_pending(struct open_file *server);
void tcp_discard_pending(struct open_file *server);
void tcp_queue_syn(struct open_file *listener, uint32_t src_ip, uint16_t src_port, uint32_t seq, uint16_t window,
                   const void *options, size_t options_len);
void tcp_set_local_port(struct open_file *file, uint16_t port);
uint32_t tcp_initial_seq(void);
void tcp_send_abortive_close(struct open_file *file);
void tcp_set_timer_active(struct open_file *file, bool active);
void tcp_note_activity(struct open_file *file, uint64_t now);
void tcp_init_keepalive(struct open_file *file, uint64_t now);
void tcp_fail(struct open_file *file, int error);
void tcp_free_rx(struct open_file *file);
uint32_t tcp_append_rx(struct open_file *file, const uint8_t *data, uint32_t len);
void tcp_ack_current_state(struct open_file *file);
uint16_t tcp_window(const struct open_file *file);
void tcp_init_rto(struct open_file *file);
uint32_t tcp_current_rto(const struct open_file *file);
uint64_t tcp_backoff_rto(const struct open_file *file, uint8_t retries);
void tcp_note_rtt_sample(struct open_file *file, uint64_t sent_tick, uint64_t now_ticks);
void tcp_tx_clear(struct open_file *file);
void tcp_tx_ack(struct open_file *file, uint32_t ack);
int tcp_tx_retransmit(struct open_file *file, uint64_t now_ticks);
