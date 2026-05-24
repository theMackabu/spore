#include "sh.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char history[HISTORY_CAP][LINE_CAP];
static int history_count;

static void put_text(const char *s) {
  (void)write(STDOUT_FILENO, s, strlen(s));
}

static void put_bytes(const char *s, size_t n) {
  (void)write(STDOUT_FILENO, s, n);
}

static void make_prompt(char *out, size_t cap) {
  char cwd[128];
  snprintf(out, cap, "%s $ ", getcwd(cwd, sizeof(cwd)) == NULL ? "/" : cwd);
}

static void redraw_line(const char *prompt, const char *line, size_t len, size_t cursor, size_t *drawn) {
  put_text("\r");
  put_text(prompt);
  put_bytes(line, len);
  if (*drawn > len) {
    for (size_t i = 0; i < *drawn - len; ++i) {
      put_text(" ");
    }
  }
  put_text("\r");
  put_text(prompt);
  put_bytes(line, cursor);
  *drawn = len;
}

static void history_add(const char *line) {
  if (line[0] == '\0') { return; }
  if (history_count > 0 && spore_streq(history[history_count - 1], line)) { return; }
  if (history_count == HISTORY_CAP) {
    memmove(history[0], history[1], sizeof(history[0]) * (HISTORY_CAP - 1));
    --history_count;
  }
  snprintf(history[history_count++], sizeof(history[0]), "%s", line);
}

int sh_read_line(char *buf, size_t cap) {
  char prompt[160];
  make_prompt(prompt, sizeof(prompt));
  put_text(prompt);

  size_t len = 0;
  size_t cursor = 0;
  size_t drawn = 0;
  int hist = history_count;
  buf[0] = '\0';

  for (;;) {
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) { return -1; }

    if (c == '\r') { c = '\n'; }
    if (c == '\n') {
      put_text("\n");
      buf[len] = '\0';
      history_add(buf);
      return 0;
    }
    if (c == 3) {
      put_text("^C\n");
      buf[0] = '\0';
      return 0;
    }
    if (c == 1) {
      cursor = 0;
      redraw_line(prompt, buf, len, cursor, &drawn);
      continue;
    }
    if (c == 5) {
      cursor = len;
      redraw_line(prompt, buf, len, cursor, &drawn);
      continue;
    }
    if (c == 11) {
      len = cursor;
      buf[len] = '\0';
      redraw_line(prompt, buf, len, cursor, &drawn);
      continue;
    }
    if (c == 21) {
      len = 0;
      cursor = 0;
      buf[0] = '\0';
      redraw_line(prompt, buf, len, cursor, &drawn);
      continue;
    }
    if (c == 0x7f || c == '\b') {
      if (cursor > 0) {
        if (cursor == len) {
          --cursor;
          --len;
          buf[len] = '\0';
          if (drawn > 0) { --drawn; }
          put_text("\b \b");
        } else {
          memmove(buf + cursor - 1, buf + cursor, len - cursor);
          --cursor;
          --len;
          buf[len] = '\0';
          redraw_line(prompt, buf, len, cursor, &drawn);
        }
      }
      continue;
    }
    if (c == 27) {
      char seq[2];
      if (read(STDIN_FILENO, &seq[0], 1) <= 0 || read(STDIN_FILENO, &seq[1], 1) <= 0) { continue; }
      if (seq[0] != '[') { continue; }
      if (seq[1] == 'D' && cursor > 0) {
        --cursor;
      } else if (seq[1] == 'C' && cursor < len) {
        ++cursor;
      } else if (seq[1] == 'A' && history_count > 0 && hist > 0) {
        --hist;
        snprintf(buf, cap, "%s", history[hist]);
        len = strlen(buf);
        cursor = len;
      } else if (seq[1] == 'B' && hist < history_count) {
        ++hist;
        if (hist == history_count) {
          len = 0;
          cursor = 0;
          buf[0] = '\0';
        } else {
          snprintf(buf, cap, "%s", history[hist]);
          len = strlen(buf);
          cursor = len;
        }
      }
      redraw_line(prompt, buf, len, cursor, &drawn);
      continue;
    }
    if ((unsigned char)c < 0x20) { continue; }
    if (len + 1 >= cap) { continue; }
    if (cursor == len) {
      buf[cursor++] = c;
      ++len;
      buf[len] = '\0';
      put_bytes(&c, 1);
      drawn = len;
    } else {
      memmove(buf + cursor + 1, buf + cursor, len - cursor);
      buf[cursor++] = c;
      ++len;
      buf[len] = '\0';
      redraw_line(prompt, buf, len, cursor, &drawn);
    }
  }
}
