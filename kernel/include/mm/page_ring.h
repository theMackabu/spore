#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct page_ring {
  uint64_t *pages;
  uint32_t capacity, head, used, allocated;
};

typedef int (*ring_copy_in)(void *source, uint8_t *dst, size_t len);
typedef int (*ring_copy_out)(void *destination, const uint8_t *src, size_t len);

void page_ring_clear(struct page_ring *ring);
int page_ring_write(struct page_ring *ring, uint32_t len, ring_copy_in copy, void *source);
int page_ring_read(struct page_ring *ring, uint32_t len, bool peek, ring_copy_out copy, void *destination);
