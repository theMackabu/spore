#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int parse_octal_mode(const char *s, mode_t *out) {
  mode_t mode = 0;
  if (*s == '\0') { return -1; }
  for (const char *p = s; *p != '\0'; ++p) {
    if (*p < '0' || *p > '7') { return -1; }
    mode = (mode_t)((mode << 3) | (mode_t)(*p - '0'));
    if (mode > 07777) { return -1; }
  }
  *out = mode;
  return 0;
}

static mode_t who_mask(const char *who, size_t len) {
  mode_t mask = 0;
  if (len == 0 || memchr(who, 'a', len) != NULL) { return 0777; }
  for (size_t i = 0; i < len; ++i) {
    switch (who[i]) {
    case 'u':
      mask |= 0700;
      break;
    case 'g':
      mask |= 0070;
      break;
    case 'o':
      mask |= 0007;
      break;
    default:
      return 0;
    }
  }
  return mask;
}

static mode_t perm_mask(const char *perms, mode_t who) {
  mode_t mask = 0;
  for (const char *p = perms; *p != '\0' && *p != ','; ++p) {
    switch (*p) {
    case 'r':
      if (who & 0700) { mask |= 0400; }
      if (who & 0070) { mask |= 0040; }
      if (who & 0007) { mask |= 0004; }
      break;
    case 'w':
      if (who & 0700) { mask |= 0200; }
      if (who & 0070) { mask |= 0020; }
      if (who & 0007) { mask |= 0002; }
      break;
    case 'x':
      if (who & 0700) { mask |= 0100; }
      if (who & 0070) { mask |= 0010; }
      if (who & 0007) { mask |= 0001; }
      break;
    default:
      return (mode_t)-1;
    }
  }
  return mask;
}

static int parse_symbolic_mode(const char *s, mode_t current, mode_t *out) {
  mode_t mode = current & 07777;
  const char *p = s;
  while (*p != '\0') {
    const char *who_start = p;
    while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
      ++p;
    }
    mode_t who = who_mask(who_start, (size_t)(p - who_start));
    if (who == 0 || (*p != '+' && *p != '-' && *p != '=')) { return -1; }
    char op = *p++;
    if (*p == '\0' || *p == ',') { return -1; }
    mode_t perms = perm_mask(p, who);
    if (perms == (mode_t)-1) { return -1; }
    while (*p != '\0' && *p != ',') {
      ++p;
    }
    if (op == '+') {
      mode |= perms;
    } else if (op == '-') {
      mode &= (mode_t)~perms;
    } else {
      mode = (mode & (mode_t)~who) | perms;
    }
    if (*p == ',') { ++p; }
  }
  *out = mode;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 3) { return usage("chmod", "MODE FILE..."); }
  mode_t mode = 0;
  bool octal = parse_octal_mode(argv[1], &mode) == 0;

  int rc = EXIT_SUCCESS;
  for (int i = 2; i < argc; ++i) {
    mode_t next = mode;
    if (!octal) {
      struct stat st;
      if (stat(argv[i], &st) != 0) {
        perror(argv[i]);
        rc = EXIT_FAILURE;
        continue;
      }
      if (parse_symbolic_mode(argv[1], st.st_mode, &next) != 0) { return usage("chmod", "MODE FILE..."); }
    }
    if (chmod(argv[i], next) != 0) {
      perror(argv[i]);
      rc = EXIT_FAILURE;
    }
  }
  return rc;
}
