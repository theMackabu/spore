#pragma once

#include <stdbool.h>
#include <stddef.h>

static inline bool str_eq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

static inline bool starts_with(const char *s, const char *prefix) {
  while (*prefix != '\0') {
    if (*s++ != *prefix++) { return false; }
  }
  return true;
}

static inline void copy_cstr(char *dst, size_t cap, const char *src) {
  if (cap == 0) { return; }
  size_t i = 0;
  if (src != NULL) {
    for (; i + 1 < cap && src[i] != '\0'; ++i) {
      dst[i] = src[i];
    }
  }
  dst[i] = '\0';
}

static inline const char *base_name(const char *path) {
  const char *base = path == NULL ? "" : path;
  for (const char *p = base; *p != '\0'; ++p) {
    if (*p == '/' && p[1] != '\0') { base = p + 1; }
  }
  return base;
}
