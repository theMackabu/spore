#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int spore_eprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = vfprintf(stderr, fmt, ap);
  va_end(ap);
  return rc;
}

int spore_usage(const char *tool, const char *usage) {
  spore_eprintf("usage: %s %s\n", tool, usage);
  return SPORE_USAGE;
}

const char *spore_basename(const char *path) {
  const char *name = path;
  for (const char *p = path; *p != '\0'; ++p) {
    if (*p == '/' && p[1] != '\0') { name = p + 1; }
  }
  return name;
}

int spore_streq(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}
