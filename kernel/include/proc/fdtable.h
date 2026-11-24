#pragma once
#include "proc/limits.h"
#include <stdint.h>
#include <stddef.h>

struct open_file;

enum { FDS_PER_CHUNK = 32, FD_CHUNKS = (MAX_FDS + FDS_PER_CHUNK - 1) / FDS_PER_CHUNK };
static_assert(FD_CHUNKS <= 32);

struct fd_chunk {
  uint32_t occupied, cloexec, reserved;
  struct open_file *files[FDS_PER_CHUNK];
};

struct fd_table {
  struct fd_chunk first;
  struct fd_chunk *more[FD_CHUNKS - 1];
  uint32_t nonempty;
};

static inline struct fd_chunk *fd_chunk_at(struct fd_table *table, unsigned index) {
  return index ? table->more[index - 1] : &table->first;
}

static inline struct open_file *fd_table_get(const struct fd_table *table, uint64_t fd) {
  if (fd >= MAX_FDS) { return NULL; }
  const struct fd_chunk *chunk = fd < FDS_PER_CHUNK ? &table->first : table->more[fd / FDS_PER_CHUNK - 1];
  return chunk ? chunk->files[fd % FDS_PER_CHUNK] : NULL;
}
