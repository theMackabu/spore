#include "fs/procfs_format.h"

void proc_append_char(char *dst, size_t cap, size_t *len, char c) {
  if (*len + 1 < cap) {
    dst[(*len)++] = c;
    dst[*len] = '\0';
  }
}

void proc_append_str(char *dst, size_t cap, size_t *len, const char *s) {
  size_t pos = *len;
  while (pos + 1 < cap && *s) {
    dst[pos++] = *s++;
  }
  if (pos != *len) {
    dst[pos] = '\0';
    *len = pos;
  }
}

void proc_append_u64_pad(char *dst, size_t cap, size_t *len, uint64_t value, size_t width) {
  char digits[33];
  char *end = digits + sizeof(digits) - 1;
  char *p = end;
  *end = '\0';
  do {
    *--p = (char)('0' + value % 10);
    value /= 10;
  } while (value);
  while (p > digits && (size_t)(end - p) < width) {
    *--p = '0';
  }
  proc_append_str(dst, cap, len, p);
}

void proc_append_u64(char *dst, size_t cap, size_t *len, uint64_t value) {
  proc_append_u64_pad(dst, cap, len, value, 0);
}

void proc_append_hex(char *dst, size_t cap, size_t *len, uint64_t value, size_t digits) {
  static const char hex[] = "0123456789abcdef";
  proc_append_str(dst, cap, len, "0x");
  while (digits) {
    --digits;
    proc_append_char(dst, cap, len, hex[(value >> (digits * 4)) & 0xf]);
  }
}
