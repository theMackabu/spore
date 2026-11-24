#pragma once

#include <stddef.h>
#include <stdint.h>

void proc_append_char(char *dst, size_t cap, size_t *len, char c);
void proc_append_str(char *dst, size_t cap, size_t *len, const char *s);
void proc_append_u64(char *dst, size_t cap, size_t *len, uint64_t value);
void proc_append_u64_pad(char *dst, size_t cap, size_t *len, uint64_t value, size_t width);
void proc_append_hex(char *dst, size_t cap, size_t *len, uint64_t value, size_t digits);
