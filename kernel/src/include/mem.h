#pragma once

#include <stddef.h>

void *kmemset(void *dst, int value, size_t len);
void *kmemcpy(void *dst, const void *src, size_t len);
int kmemcmp(const void *a, const void *b, size_t len);
size_t kstrlen(const char *s);
