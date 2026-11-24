#pragma once

#include <stdarg.h>
#include <stdint.h>

void klog_handoff(void);
void klog_init_console(uint64_t hhdm_offset, unsigned cpus, const char *loader);
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...);
uint64_t klog_copy(char *dst, uint64_t cap);
