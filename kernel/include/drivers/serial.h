#pragma once
#include <stdint.h>
#include <stdbool.h>

void serial_putc(char c);
bool serial_getc(char *out);
void serial_get_winsize(uint16_t *rows, uint16_t *cols);
