#include <spore.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int eprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = vfprintf(stderr, fmt, ap);
  va_end(ap);
  return rc;
}

int usage(const char *tool, const char *usage) {
  eprintf("usage: %s %s\n", tool, usage);
  return EXIT_USAGE;
}

const char *basename(const char *path) {
  const char *name = path;
  for (const char *p = path; *p != '\0'; ++p) {
    if (*p == '/' && p[1] != '\0') { name = p + 1; }
  }
  return name;
}

bool streq(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

void format_ipv4(uint32_t ip, char *out, size_t cap) {
  snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(ip & 0xffu), (unsigned)((ip >> 8) & 0xffu),
           (unsigned)((ip >> 16) & 0xffu), (unsigned)((ip >> 24) & 0xffu));
}

bool net_config_get(struct net_config *out) {
  return out != NULL && syscall(SYS_spore_net_config, 0, out) == 0;
}

bool net_config_set(const struct net_config *cfg) {
  return cfg != NULL && syscall(SYS_spore_net_config, 1, cfg) == 0;
}
