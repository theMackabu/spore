#pragma once

#include <stdbool.h>
#include <stdint.h>

struct open_file;
struct uring_pending;
struct uring {
  struct uring_pending *requests, *last, *dependency, *new_request;
  unsigned drains;
  bool linked;
  struct open_file *next_active, **active_link;
  uint64_t rings, sqes;
  uint32_t entries, cq_entries, sq_array_offset;
  uint32_t sq_head, cq_tail, dropped, pending, waiters;
  struct uring_group *groups;
  struct uring_buffer *buffers;
  uint32_t nr_buffers;
  struct uring_slot *files;
  struct uring_extra *overflow_head, *overflow_tail;
  unsigned backlog, pinned_pages, timeout_cqes;
  bool closing, restricted;
  uint8_t sq_shift, cq_shift, allowed_flags, required_flags;
  uint64_t allowed_ops, allowed_register;
  int issuer;
  uint32_t nr_files;
  struct open_file *eventfd;
  bool eventfd_async, disabled;
};

void uring_service_events(void);
void uring_release(struct uring *ring);
int uring_poll(struct uring *ring, int events);
int64_t uring_map(struct uring *ring, uint64_t base, uint64_t size, uint64_t off, uint32_t prot, uint32_t flags);
