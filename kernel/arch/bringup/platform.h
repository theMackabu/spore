#pragma once

void console_init(void);
void console_putc(char c);
int console_getc(void);
[[noreturn]] void platform_poweroff(void);
int console_try_getc(void);

void platform_wait_tick(void);
void platform_rearm_timer(void);
