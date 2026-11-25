#pragma once

#include <stdbool.h>
#include <stdint.h>

void pl011_init(uint64_t hhdm_offset);
void pl011_enable_rx_irq(void);
bool pl011_handle_irq(void);
bool pl011_poll_rx(void);
bool pl011_take_resize(void);
