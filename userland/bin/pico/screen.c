#include "pico.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void out(const char *s) {
  (void)write(STDOUT_FILENO, s, strlen(s));
}

static void out_n(const char *s, size_t n) {
  (void)write(STDOUT_FILENO, s, n);
}

void pico_set_status(struct pico_editor *ed, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ed->status, sizeof(ed->status), fmt, ap);
  va_end(ap);
}

static void draw_bar(const struct pico_editor *ed, const char *s, bool reverse) {
  size_t cols = pico_text_cols(ed);
  if (reverse) { out("\033[7m"); }
  size_t len = strlen(s);
  if (len > cols) { len = cols; }
  out_n(s, len);
  for (size_t i = len; i < cols; ++i) {
    out(" ");
  }
  if (reverse) { out("\033[m"); }
}

static void draw_text_row(const struct pico_editor *ed, size_t screen_row) {
  size_t row = ed->row_offset + screen_row;
  size_t cols = pico_text_cols(ed);
  if (row >= ed->line_count) {
    out("~");
    for (size_t i = 1; i < cols; ++i) {
      out(" ");
    }
    return;
  }
  size_t col = 0;
  for (const char *p = ed->lines[row]; *p != '\0' && col < cols; ++p) {
    if (*p == '\t') {
      for (int i = 0; i < PICO_TAB_WIDTH && col < cols; ++i, ++col) {
        out(" ");
      }
    } else {
      out_n(p, 1);
      ++col;
    }
  }
  for (; col < cols; ++col) {
    out(" ");
  }
}

void pico_redraw(struct pico_editor *ed) {
  pico_clamp_cursor(ed);
  out("\033[?25l\033[H");

  char title[PICO_STATUS_CAP];
  snprintf(title, sizeof(title), "  Spore Pico  %s%s  line %u/%u", ed->path, ed->dirty ? " [modified]" : "",
           (unsigned)(ed->cursor_row + 1), (unsigned)ed->line_count);
  draw_bar(ed, title, true);

  size_t rows = pico_text_rows(ed);
  for (size_t i = 0; i < rows; ++i) {
    out("\r\n");
    draw_text_row(ed, i);
  }

  out("\r\n");
  draw_bar(ed, ed->status[0] == '\0' ? "^G Help   ^O Save   ^K Cut   ^U Uncut   ^X Exit" : ed->status, true);

  char pos[48];
  snprintf(pos, sizeof(pos), "\033[%u;%uH\033[?25h", (unsigned)(2 + ed->cursor_row - ed->row_offset),
           (unsigned)(ed->cursor_col + 1));
  out(pos);
}

void pico_show_help(struct pico_editor *ed) {
  pico_clear_screen();
  puts("Spore Pico help");
  puts("");
  puts("  Arrow keys     move cursor");
  puts("  Down at EOF    create a blank line");
  puts("  Enter          split line");
  puts("  Backspace      delete before cursor");
  puts("  Delete         delete at cursor");
  puts("  Ctrl-O         write file");
  puts("  Ctrl-X         exit");
  puts("  Ctrl-K         cut current line");
  puts("  Ctrl-U         paste cut line");
  puts("");
  puts("Press any key to return.");
  (void)pico_read_key();
  ed->status[0] = '\0';
}
