#include "lib/mem.h"

#include <stdint.h>

typedef uint64_t word_t __attribute__((may_alias));

void *kmemset(void *dst, int value, size_t len) {
  uint8_t *p = dst;
  if (len >= sizeof(word_t)) {
    while ((uintptr_t)p % sizeof(word_t) != 0) {
      *p++ = (uint8_t)value;
      --len;
    }
    word_t pattern = UINT64_C(0x0101010101010101) * (uint8_t)value;
    while (len >= sizeof(word_t)) {
      *(word_t *)p = pattern;
      p += sizeof(word_t);
      len -= sizeof(word_t);
    }
  }
  for (size_t i = 0; i < len; ++i) {
    p[i] = (uint8_t)value;
  }
  return dst;
}

void *kmemcpy(void *dst, const void *src, size_t len) {
  uint8_t *d = dst;
  const uint8_t *s = src;
  if (len >= sizeof(word_t) && ((uintptr_t)d ^ (uintptr_t)s) % sizeof(word_t) == 0) {
    while ((uintptr_t)d % sizeof(word_t) != 0) {
      *d++ = *s++;
      --len;
    }
#pragma GCC unroll 4
    while (len >= sizeof(word_t)) {
      *(word_t *)d = *(const word_t *)s;
      d += sizeof(word_t);
      s += sizeof(word_t);
      len -= sizeof(word_t);
    }
  }
  for (size_t i = 0; i < len; ++i) {
    d[i] = s[i];
  }
  return dst;
}

#if !__STDC_HOSTED__
void *memcpy(void *dst, const void *src, size_t len) {
  return kmemcpy(dst, src, len);
}

void *memset(void *dst, int value, size_t len) {
  return kmemset(dst, value, len);
}
#endif

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
