#pragma once

#include <stdarg.h>
#include <stdint.h>

void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);
uint64_t klog_copy(char *dst, uint64_t cap);
