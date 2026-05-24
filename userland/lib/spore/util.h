#pragma once

#include <stddef.h>
#include <stdlib.h>

enum exit_code {
  EXIT_USAGE = 64,
};

int eprintf(const char *fmt, ...);
int usage(const char *tool, const char *usage);
const char *basename(const char *path);
int streq(const char *a, const char *b);
