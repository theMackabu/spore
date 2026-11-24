#pragma once

#include "../internal.h"

void tcp_free_rx(struct open_file *file);
uint32_t tcp_append_rx(struct open_file *file, const uint8_t *data, uint32_t len);
void tcp_ack_current_state(struct open_file *file);
