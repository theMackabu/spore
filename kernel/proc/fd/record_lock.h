#pragma once

#include <stdbool.h>
#include <stdint.h>

struct open_file;

enum record_lock_type { RECORD_READ, RECORD_WRITE, RECORD_UNLOCK };

struct record_lock_range {
  uint64_t start, end;
  int owner;
  enum record_lock_type type;
};

int cell_record_lock(struct open_file *file, struct record_lock_range *range, bool query);
void cell_record_unlock_file(int owner, const struct open_file *file);
