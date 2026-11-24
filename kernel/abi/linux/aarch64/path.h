#pragma once

#include "proc/limits.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct syscall_path {
  char visible[CELL_PATH_MAX];
  char resolved[CELL_PATH_MAX];
  bool trailing_slash;
};

int64_t syscall_read_path_at(uint64_t dirfd, uint64_t path_addr, struct syscall_path *path);
int64_t syscall_resolve_path_at(uint64_t dirfd, const char *raw, char *out, size_t cap);
int64_t syscall_copy_resolved_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap);
