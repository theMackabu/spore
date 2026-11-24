#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pool_page;
struct object_pool {
  struct pool_page *available;
  size_t stride;
  size_t count;
  size_t limit;
  uint64_t free_mask;
};

bool pool_init(struct object_pool *pool, size_t size, size_t limit);
void *pool_alloc(struct object_pool *pool);
void pool_free(struct object_pool *pool, void *object);
