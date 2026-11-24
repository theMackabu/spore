#pragma once

#include <stdbool.h>
#include <stdint.h>

struct uring {
  uint64_t rings, sqes;
  uint32_t entries, cq_entries, sq_array_offset;
  uint32_t sq_head, cq_tail, dropped;
};

void uring_release(struct uring *ring);
int uring_poll(struct uring *ring, int events);
int64_t uring_map(struct uring *ring, uint64_t base, uint64_t size, uint64_t off, uint32_t prot, uint32_t flags);
