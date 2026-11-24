#pragma once

void console_init(void);
void console_putc(char c);
int console_getc(void);
[[noreturn]] void platform_poweroff(void);
