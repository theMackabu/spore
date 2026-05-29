#include "msh.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

enum {
  COMPLETE_CAP = 96,
  COMPLETE_NAME_CAP = 160,
  COMPLETE_PATH_CAP = 256,
};

struct completion {
  char text[COMPLETE_NAME_CAP];
  bool dir;
};

static const char *builtins[] = {
  ".",    "cd",  "command", "confine", "exit", "export", "fg",   "help",  "jobs",
  "kill", "pwd", "runc",    "select",  "set",  "source", "test", "unset", "wait",
};

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

static bool is_word_break(char c) {
  return c == ' ' || c == '\t' || c == '|' || c == ';' || c == '&' || c == '<' || c == '>';
}

static bool starts_with(const char *s, const char *prefix) {
  size_t n = strlen(prefix);
  return strncmp(s, prefix, n) == 0;
}

static bool completion_exists(const struct completion *items, size_t count, const char *text) {
  for (size_t i = 0; i < count; ++i) {
    if (streq(items[i].text, text)) { return true; }
  }
  return false;
}

static void add_completion(struct completion *items, size_t *count, const char *text, bool dir) {
  if (*count >= COMPLETE_CAP || text[0] == '\0' || completion_exists(items, *count, text)) { return; }
  snprintf(items[*count].text, sizeof(items[*count].text), "%s", text);
  items[*count].dir = dir;
  ++*count;
}

static bool command_position(const char *buf, size_t word_start) {
  size_t i = word_start;
  while (i > 0 && (buf[i - 1] == ' ' || buf[i - 1] == '\t')) {
    --i;
  }
  if (i == 0) { return true; }
  char prev = buf[i - 1];
  return prev == '|' || prev == ';' || prev == '&';
}

static void split_completion_word(const char *word, char *dir, size_t dir_cap, char *prefix, size_t prefix_cap,
                                  char *insert_dir, size_t insert_dir_cap) {
  const char *slash = strrchr(word, '/');
  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') { home = "/"; }

  if (slash == NULL) {
    snprintf(dir, dir_cap, ".");
    snprintf(insert_dir, insert_dir_cap, "");
    snprintf(prefix, prefix_cap, "%s", word);
    return;
  }

  size_t dir_len = (size_t)(slash - word);
  if (dir_len == 0) {
    snprintf(dir, dir_cap, "/");
    snprintf(insert_dir, insert_dir_cap, "/");
  } else if (word[0] == '~' && (word[1] == '/' || word[1] == '\0')) {
    snprintf(dir, dir_cap, "%s%.*s", home, (int)(dir_len - 1), word + 1);
    snprintf(insert_dir, insert_dir_cap, "%.*s/", (int)dir_len, word);
  } else {
    snprintf(dir, dir_cap, "%.*s", (int)dir_len, word);
    snprintf(insert_dir, insert_dir_cap, "%.*s/", (int)dir_len, word);
  }
  snprintf(prefix, prefix_cap, "%s", slash + 1);
}

static void join_completion_stat_path(char *out, size_t cap, const char *dir, const char *name) {
  if (streq(dir, "/")) {
    snprintf(out, cap, "/%s", name);
  } else {
    snprintf(out, cap, "%s/%s", dir, name);
  }
}

static void collect_path_matches(const char *word, struct completion *items, size_t *count) {
  char dir[COMPLETE_PATH_CAP];
  char prefix[COMPLETE_NAME_CAP];
  char insert_dir[COMPLETE_PATH_CAP];
  split_completion_word(word, dir, sizeof(dir), prefix, sizeof(prefix), insert_dir, sizeof(insert_dir));

  DIR *d = opendir(dir);
  if (d == NULL) { return; }
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (!starts_with(ent->d_name, prefix)) { continue; }
    if (ent->d_name[0] == '.' && prefix[0] != '.') { continue; }
    char stat_path[COMPLETE_PATH_CAP];
    join_completion_stat_path(stat_path, sizeof(stat_path), dir, ent->d_name);
    struct stat st;
    bool is_dir = stat(stat_path, &st) == 0 && S_ISDIR(st.st_mode);
    char text[COMPLETE_NAME_CAP];
    snprintf(text, sizeof(text), "%s%s", insert_dir, ent->d_name);
    add_completion(items, count, text, is_dir);
  }
  closedir(d);
}

static void collect_command_matches(const char *word, struct completion *items, size_t *count) {
  for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
    if (starts_with(builtins[i], word)) { add_completion(items, count, builtins[i], false); }
  }

  const char *path = getenv("PATH");
  if (path == NULL || path[0] == '\0') { path = "/bin"; }
  const char *p = path;
  while (*p != '\0') {
    const char *end = strchr(p, ':');
    size_t len = end == NULL ? strlen(p) : (size_t)(end - p);
    char dir[COMPLETE_PATH_CAP];
    if (len == 0) {
      snprintf(dir, sizeof(dir), ".");
    } else {
      snprintf(dir, sizeof(dir), "%.*s", (int)len, p);
    }
    DIR *d = opendir(dir);
    if (d != NULL) {
      struct dirent *ent;
      while ((ent = readdir(d)) != NULL) {
        if (!starts_with(ent->d_name, word)) { continue; }
        char stat_path[COMPLETE_PATH_CAP];
        join_completion_stat_path(stat_path, sizeof(stat_path), dir, ent->d_name);
        if (access(stat_path, X_OK) == 0) { add_completion(items, count, ent->d_name, false); }
      }
      closedir(d);
    }
    if (end == NULL) { break; }
    p = end + 1;
  }
}

static int compare_completion(const void *lhs, const void *rhs) {
  const struct completion *a = lhs;
  const struct completion *b = rhs;
  return strcmp(a->text, b->text);
}

static size_t common_prefix_len(const struct completion *items, size_t count) {
  if (count == 0) { return 0; }
  size_t n = strlen(items[0].text);
  for (size_t i = 1; i < count; ++i) {
    size_t j = 0;
    while (j < n && items[i].text[j] != '\0' && items[i].text[j] == items[0].text[j]) {
      ++j;
    }
    n = j;
  }
  return n;
}

static void replace_word(char *buf, size_t cap, size_t *len, size_t *cursor, size_t start, size_t end,
                         const char *text) {
  size_t text_len = strlen(text);
  if (*len - (end - start) + text_len >= cap) { return; }
  memmove(buf + start + text_len, buf + end, *len - end + 1);
  memcpy(buf + start, text, text_len);
  *len = *len - (end - start) + text_len;
  *cursor = start + text_len;
}

static void list_completions(const struct completion *items, size_t count) {
  putchar('\n');
  for (size_t i = 0; i < count; ++i) {
    fputs(items[i].text, stdout);
    if (items[i].dir) { putchar('/'); }
    putchar(i + 1 == count ? '\n' : ' ');
  }
}

static void complete_line(const char *prompt, char *buf, size_t cap, size_t *len, size_t *cursor) {
  size_t start = *cursor;
  while (start > 0 && !is_word_break(buf[start - 1])) {
    --start;
  }

  char word[COMPLETE_NAME_CAP];
  size_t word_len = *cursor - start;
  if (word_len >= sizeof(word)) { word_len = sizeof(word) - 1; }
  memcpy(word, buf + start, word_len);
  word[word_len] = '\0';

  struct completion items[COMPLETE_CAP];
  size_t count = 0;
  if (command_position(buf, start) && strchr(word, '/') == NULL) { collect_command_matches(word, items, &count); }
  collect_path_matches(word, items, &count);
  if (count == 0) {
    putchar('\a');
    fflush(stdout);
    return;
  }
  qsort(items, count, sizeof(items[0]), compare_completion);

  size_t common = common_prefix_len(items, count);
  if (count == 1) {
    char replacement[COMPLETE_NAME_CAP];
    snprintf(replacement, sizeof(replacement), "%s%s", items[0].text, items[0].dir ? "/" : " ");
    replace_word(buf, cap, len, cursor, start, *cursor, replacement);
    redraw_input(prompt, buf, *len, *cursor);
    return;
  }

  if (common > word_len) {
    char replacement[COMPLETE_NAME_CAP];
    snprintf(replacement, sizeof(replacement), "%.*s", (int)common, items[0].text);
    replace_word(buf, cap, len, cursor, start, *cursor, replacement);
    redraw_input(prompt, buf, *len, *cursor);
    return;
  }

  list_completions(items, count);
  redraw_input(prompt, buf, *len, *cursor);
}

static int read_prompt_line(const char *prompt, char *buf, size_t cap, bool use_history) {
  if (cap == 0) { return -1; }
  struct termios saved;
  if (tcgetattr(STDIN_FILENO, &saved) != 0) { return read_fallback_line(prompt, buf, cap); }

  struct termios raw = saved;
  raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) { return read_fallback_line(prompt, buf, cap); }

  buf[0] = '\0';
  size_t len = 0;
  size_t cursor = 0;
  size_t history_cursor = sh_history_count();
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
    if (c == '\t') {
      complete_line(prompt, buf, cap, &len, &cursor);
      continue;
    }
    if (c == 0x1b) {
      char seq[2];
      if (read(STDIN_FILENO, &seq[0], 1) <= 0 || read(STDIN_FILENO, &seq[1], 1) <= 0) { continue; }
      if (seq[0] != '[') { continue; }
      if (seq[1] == 'A' && use_history && history_cursor > 0) {
        replace_line(buf, cap, &len, &cursor, sh_history_get(--history_cursor));
        redraw_input(prompt, buf, len, cursor);
      } else if (seq[1] == 'B' && use_history && history_cursor < sh_history_count()) {
        ++history_cursor;
        replace_line(buf, cap, &len, &cursor,
                     history_cursor == sh_history_count() ? "" : sh_history_get(history_cursor));
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
  sh_history_add(buf);
  return 0;
}
