#pragma once

#include "boot_info.h"

#include <stdbool.h>
#include <stdint.h>

bool framebuffer_init(const struct spore_boot_info *boot);
bool framebuffer_ready(void);
void framebuffer_get_winsize(uint16_t *rows, uint16_t *cols);
void framebuffer_set_flush(void (*flush)(void));
void framebuffer_putc(char c);
void framebuffer_flush(void);
void framebuffer_tick(void);
