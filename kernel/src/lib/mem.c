#include "mem.h"

#include <stdint.h>

void *kmemset(void *dst, int value, size_t len) {
  uint8_t *p = dst;
  for (size_t i = 0; i < len; ++i) {
    p[i] = (uint8_t)value;
  }
  return dst;
}

void *kmemcpy(void *dst, const void *src, size_t len) {
  uint8_t *d = dst;
  const uint8_t *s = src;
  for (size_t i = 0; i < len; ++i) {
    d[i] = s[i];
  }
  return dst;
}

void *memcpy(void *dst, const void *src, size_t len) {
  return kmemcpy(dst, src, len);
}

void *memset(void *dst, int value, size_t len) {
  return kmemset(dst, value, len);
}

int kmemcmp(const void *a, const void *b, size_t len) {
  const uint8_t *aa = a;
  const uint8_t *bb = b;
  for (size_t i = 0; i < len; ++i) {
    if (aa[i] != bb[i]) { return (int)aa[i] - (int)bb[i]; }
  }
  return 0;
}

size_t kstrlen(const char *s) {
  size_t len = 0;
  while (s[len] != '\0') {
    ++len;
  }
  return len;
}
