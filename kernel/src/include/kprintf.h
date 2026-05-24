#pragma once

#include <stdarg.h>
#include <stdint.h>

void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);
