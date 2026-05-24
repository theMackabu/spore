#include "msh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool path_has_prefix_dir(const char *path, const char *prefix) {
  size_t n = strlen(prefix);
  return n > 0 && strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

static void append_char(char *out, size_t cap, size_t *len, char c) {
  if (*len + 1 >= cap) { return; }
  out[(*len)++] = c;
  out[*len] = '\0';
}

static void append_text(char *out, size_t cap, size_t *len, const char *text) {
  while (*text != '\0') {
    append_char(out, cap, len, *text++);
  }
}

static void format_path(char *out, size_t cap, const char *path) {
  const char *home = getenv("HOME");
  if (home != NULL && home[0] == '/' && path_has_prefix_dir(path, home)) {
    if (path[strlen(home)] == '\0') {
      snprintf(out, cap, "~");
    } else {
      snprintf(out, cap, "~%s", path + strlen(home));
    }
    return;
  }
  snprintf(out, cap, "%s", path);
}

static void format_path_base(char *out, size_t cap, const char *path) {
  char folded[128];
  format_path(folded, sizeof(folded), path);
  const char *base = strrchr(folded, '/');
  if (base != NULL && base[1] != '\0') {
    snprintf(out, cap, "%s", base + 1);
  } else {
    snprintf(out, cap, "%s", folded);
  }
}

static void current_user(char *out, size_t cap) {
  struct user_entry user;
  if (user_by_uid((unsigned)geteuid(), &user)) {
    snprintf(out, cap, "%s", user.name);
    return;
  }
  snprintf(out, cap, "%u", (unsigned)geteuid());
}

static void current_hostname(char *out, size_t cap) {
  if (gethostname(out, cap) != 0 || out[0] == '\0') {
    snprintf(out, cap, "spore.local");
  } else {
    out[cap - 1] = '\0';
  }
}

void sh_expand_prompt(const char *src, char *out, size_t cap) {
  if (cap == 0) { return; }
  out[0] = '\0';
  char cwd[128];
  const char *path = getcwd(cwd, sizeof(cwd)) == NULL ? "/" : cwd;
  char display_path[128];
  char display_base[128];
  char user[32];
  char host[64];
  format_path(display_path, sizeof(display_path), path);
  format_path_base(display_base, sizeof(display_base), path);
  current_user(user, sizeof(user));
  current_hostname(host, sizeof(host));

  size_t len = 0;
  for (const char *p = src; *p != '\0'; ++p) {
    if (*p != '\\') {
      append_char(out, cap, &len, *p);
      continue;
    }
    ++p;
    if (*p == '\0') {
      append_char(out, cap, &len, '\\');
      break;
    }
    switch (*p) {
    case 'u':
      append_text(out, cap, &len, user);
      break;
    case 'h':
      append_text(out, cap, &len, host);
      break;
    case 'w':
      append_text(out, cap, &len, display_path);
      break;
    case 'W':
      append_text(out, cap, &len, display_base);
      break;
    case '$':
      append_char(out, cap, &len, geteuid() == 0 ? '#' : '$');
      break;
    case 'e':
      append_char(out, cap, &len, '\033');
      break;
    case '\\':
      append_char(out, cap, &len, '\\');
      break;
    case '[':
    case ']':
      break;
    default:
      append_char(out, cap, &len, *p);
      break;
    }
  }
}

static void make_prompt(char *out, size_t cap) {
  const char *ps1 = getenv("PS1");
  if (ps1 != NULL) {
    sh_expand_prompt(ps1, out, cap);
    return;
  }
  snprintf(out, cap, "msh%c ", geteuid() == 0 ? '#' : '$');
}

static void make_secondary_prompt(char *out, size_t cap) {
  const char *ps2 = getenv("PS2");
  sh_expand_prompt(ps2 == NULL ? "> " : ps2, out, cap);
}

static bool line_has_trailing_backslash(const char *buf) {
  size_t len = strlen(buf);
  size_t slashes = 0;
  while (len > 0 && buf[len - 1] == '\\') {
    ++slashes;
    --len;
  }
  return (slashes % 2) != 0;
}

static bool line_needs_more(const char *buf) {
  bool single = false;
  bool dbl = false;
  for (const char *p = buf; *p != '\0'; ++p) {
    if (*p == '\\' && p[1] != '\0') {
      ++p;
    } else if (*p == '\'' && !dbl) {
      single = !single;
    } else if (*p == '"' && !single) {
      dbl = !dbl;
    }
  }
  return single || dbl || line_has_trailing_backslash(buf);
}

static bool append_continuation(char *buf, size_t cap, const char *more) {
  size_t len = strlen(buf);
  if (line_has_trailing_backslash(buf) && len > 0) {
    buf[--len] = '\0';
  } else if (len + 1 < cap) {
    buf[len++] = '\n';
    buf[len] = '\0';
  } else {
    return false;
  }
  size_t more_len = strlen(more);
  if (len + more_len >= cap) { return false; }
  memcpy(buf + len, more, more_len + 1);
  return true;
}

int sh_read_line(char *buf, size_t cap) {
  char prompt[160];
  make_prompt(prompt, sizeof(prompt));
  fputs(prompt, stdout);
  fflush(stdout);
  if (fgets(buf, (int)cap, stdin) == NULL) { return -1; }
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
    buf[--len] = '\0';
  }
  while (line_needs_more(buf)) {
    char more[LINE_CAP];
    char ps2[64];
    make_secondary_prompt(ps2, sizeof(ps2));
    fputs(ps2, stdout);
    fflush(stdout);
    if (fgets(more, (int)sizeof(more), stdin) == NULL) { return -1; }
    size_t more_len = strlen(more);
    while (more_len > 0 && (more[more_len - 1] == '\n' || more[more_len - 1] == '\r')) {
      more[--more_len] = '\0';
    }
    if (!append_continuation(buf, cap, more)) {
      eprintf("sh: line too long\n");
      return 0;
    }
  }
  return 0;
}
