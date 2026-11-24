#pragma once

#include "../internal.h"

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
