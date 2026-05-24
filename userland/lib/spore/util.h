#pragma once

#include <stddef.h>

enum spore_exit {
  SPORE_OK = 0,
  SPORE_USAGE = 64,
  SPORE_ERROR = 1,
};

int spore_eprintf(const char *fmt, ...);
int spore_usage(const char *tool, const char *usage);
const char *spore_basename(const char *path);
int spore_streq(const char *a, const char *b);
