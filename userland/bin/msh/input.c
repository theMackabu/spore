#include "msh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static char history[HISTORY_CAP][LINE_CAP];
static size_t history_len;

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

static void move_cursor_left(size_t n) {
  if (n == 0) { return; }
  printf("\033[%zuD", n);
}

static void redraw_input(const char *prompt, const char *line, size_t len, size_t cursor) {
  printf("\r\033[K%s", prompt);
  if (len > 0) { fwrite(line, 1, len, stdout); }
  if (cursor < len) { move_cursor_left(len - cursor); }
  fflush(stdout);
}

static void history_add(const char *line) {
  if (line[0] == '\0') { return; }
  if (history_len > 0 && strcmp(history[history_len - 1], line) == 0) { return; }
  if (history_len == HISTORY_CAP) {
    for (size_t i = 1; i < history_len; ++i) {
      memcpy(history[i - 1], history[i], sizeof(history[0]));
    }
    --history_len;
  }
  snprintf(history[history_len++], sizeof(history[0]), "%s", line);
}

static int read_fallback_line(const char *prompt, char *buf, size_t cap) {
  fputs(prompt, stdout);
  fflush(stdout);
  if (fgets(buf, (int)cap, stdin) == NULL) { return -1; }
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
    buf[--len] = '\0';
  }
  return 0;
}

static void insert_char(char *buf, size_t cap, size_t *len, size_t *cursor, char c) {
  if (*len + 1 >= cap) { return; }
  for (size_t i = *len; i > *cursor; --i) {
    buf[i] = buf[i - 1];
  }
  buf[(*cursor)++] = c;
  ++*len;
  buf[*len] = '\0';
}

static void erase_before_cursor(char *buf, size_t *len, size_t *cursor) {
  if (*cursor == 0) { return; }
  for (size_t i = *cursor - 1; i + 1 < *len; ++i) {
    buf[i] = buf[i + 1];
  }
  --*cursor;
  --*len;
  buf[*len] = '\0';
}

static void replace_line(char *buf, size_t cap, size_t *len, size_t *cursor, const char *text) {
  snprintf(buf, cap, "%s", text);
  *len = strlen(buf);
  *cursor = *len;
}

static int read_prompt_line(const char *prompt, char *buf, size_t cap, bool use_history) {
  if (cap == 0) { return -1; }
  struct termios saved;
  if (tcgetattr(STDIN_FILENO, &saved) != 0) { return read_fallback_line(prompt, buf, cap); }

  struct termios raw = saved;
  raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) { return read_fallback_line(prompt, buf, cap); }

  buf[0] = '\0';
  size_t len = 0;
  size_t cursor = 0;
  size_t history_cursor = history_len;
  fputs(prompt, stdout);
  fflush(stdout);

  for (;;) {
    char c;
    if (read(STDIN_FILENO, &c, 1) <= 0) {
      (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
      return -1;
    }
    if (c == '\r' || c == '\n') {
      putchar('\n');
      if (use_history) { history_add(buf); }
      (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
      return 0;
    }
    if (c == 3) {
      fputs("^C\n", stdout);
      buf[0] = '\0';
      (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved);
      return 0;
    }
    if (c == '\b' || c == 0x7f) {
      erase_before_cursor(buf, &len, &cursor);
      redraw_input(prompt, buf, len, cursor);
      continue;
    }
    if (c == 0x1b) {
      char seq[2];
      if (read(STDIN_FILENO, &seq[0], 1) <= 0 || read(STDIN_FILENO, &seq[1], 1) <= 0) { continue; }
      if (seq[0] != '[') { continue; }
      if (seq[1] == 'A' && use_history && history_cursor > 0) {
        replace_line(buf, cap, &len, &cursor, history[--history_cursor]);
        redraw_input(prompt, buf, len, cursor);
      } else if (seq[1] == 'B' && use_history && history_cursor < history_len) {
        ++history_cursor;
        replace_line(buf, cap, &len, &cursor, history_cursor == history_len ? "" : history[history_cursor]);
        redraw_input(prompt, buf, len, cursor);
      } else if (seq[1] == 'C' && cursor < len) {
        ++cursor;
        fputs("\033[C", stdout);
        fflush(stdout);
      } else if (seq[1] == 'D' && cursor > 0) {
        --cursor;
        fputs("\033[D", stdout);
        fflush(stdout);
      }
      continue;
    }
    if ((unsigned char)c < 0x20 && c != '\t') { continue; }
    insert_char(buf, cap, &len, &cursor, c);
    if (cursor == len) {
      putchar(c);
      fflush(stdout);
    } else {
      redraw_input(prompt, buf, len, cursor);
    }
  }
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
  if (read_prompt_line(prompt, buf, cap, true) != 0) { return -1; }
  while (line_needs_more(buf)) {
    char more[LINE_CAP];
    char ps2[64];
    make_secondary_prompt(ps2, sizeof(ps2));
    if (read_prompt_line(ps2, more, sizeof(more), false) != 0) { return -1; }
    if (!append_continuation(buf, cap, more)) {
      eprintf("sh: line too long\n");
      return 0;
    }
  }
  return 0;
}
