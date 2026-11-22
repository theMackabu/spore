#pragma once

#include <stdbool.h>
#include <stdint.h>

void pl011_init(uint64_t hhdm_offset);
void pl011_putc(char c);
void pl011_enable_rx_irq(void);
bool pl011_handle_irq(void);
bool pl011_poll_rx(void);
bool pl011_getc(char *out);
void pl011_get_winsize(uint16_t *rows, uint16_t *cols);
