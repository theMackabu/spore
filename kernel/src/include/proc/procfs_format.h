#pragma once

#include <stddef.h>
#include <stdint.h>

static inline void proc_append_char(char *dst, size_t cap, size_t *len, char c) {
  if (*len + 1 < cap) {
    dst[*len] = c;
    ++*len;
    dst[*len] = '\0';
  }
}

static inline void proc_append_str(char *dst, size_t cap, size_t *len, const char *s) {
  while (*s != '\0') {
    proc_append_char(dst, cap, len, *s++);
  }
}

static inline void proc_append_u64(char *dst, size_t cap, size_t *len, uint64_t value) {
  char tmp[32];
  size_t n = 0;
  if (value == 0) {
    proc_append_char(dst, cap, len, '0');
    return;
  }
  while (value != 0 && n < sizeof(tmp)) {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  }
  while (n > 0) {
    proc_append_char(dst, cap, len, tmp[--n]);
  }
}

static inline void proc_append_u64_pad(char *dst, size_t cap, size_t *len, uint64_t value, size_t width) {
  char tmp[32];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  } while (value != 0 && n < sizeof(tmp));
  while (n < width && n < sizeof(tmp)) {
    tmp[n++] = '0';
  }
  while (n > 0) {
    proc_append_char(dst, cap, len, tmp[--n]);
  }
}

static inline void proc_append_hex(char *dst, size_t cap, size_t *len, uint64_t value, size_t digits) {
  static const char hex[] = "0123456789abcdef";
  proc_append_str(dst, cap, len, "0x");
  for (size_t i = 0; i < digits; ++i) {
    size_t shift = (digits - i - 1) * 4;
    proc_append_char(dst, cap, len, hex[(value >> shift) & 0xf]);
  }
}

