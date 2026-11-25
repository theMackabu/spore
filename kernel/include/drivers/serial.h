#pragma once
#include <stdbool.h>
#include <stdint.h>

void serial_putc(char c);
void serial_flush_input(void);
bool serial_getc(char *out);
void serial_get_winsize(uint16_t *rows, uint16_t *cols);
