#include "pico.h"

#include <stdio.h>
#include <string.h>

size_t pico_text_rows(const struct pico_editor *ed) {
  return ed->screen_rows > 2 ? ed->screen_rows - 2 : 1;
}

size_t pico_text_cols(const struct pico_editor *ed) {
  return ed->screen_cols > PICO_RIGHT_GUTTER + 1 ? ed->screen_cols - PICO_RIGHT_GUTTER : 1;
}

void pico_clamp_cursor(struct pico_editor *ed) {
  if (ed->line_count == 0) { ed->line_count = 1; }
  if (ed->cursor_row >= ed->line_count) { ed->cursor_row = ed->line_count - 1; }
  size_t len = pico_line_len(ed, ed->cursor_row);
  if (ed->cursor_col > len) { ed->cursor_col = len; }
  size_t rows = pico_text_rows(ed);
  if (ed->cursor_row < ed->row_offset) { ed->row_offset = ed->cursor_row; }
  if (ed->cursor_row >= ed->row_offset + rows) { ed->row_offset = ed->cursor_row - rows + 1; }
}

size_t pico_line_len(const struct pico_editor *ed, size_t row) {
  return row < ed->line_count ? strlen(ed->lines[row]) : 0;
}

bool pico_insert_empty_line(struct pico_editor *ed, size_t row) {
  if (ed->line_count >= PICO_MAX_LINES || row > ed->line_count) {
    pico_set_status(ed, "Buffer is full");
    return false;
  }
  for (size_t i = ed->line_count; i > row; --i) {
    memcpy(ed->lines[i], ed->lines[i - 1], sizeof(ed->lines[i]));
  }
  ed->lines[row][0] = '\0';
  ++ed->line_count;
  ed->dirty = true;
  return true;
}

static void delete_line_at(struct pico_editor *ed, size_t row) {
  if (ed->line_count == 0 || row >= ed->line_count) { return; }
  for (size_t i = row; i + 1 < ed->line_count; ++i) {
    memcpy(ed->lines[i], ed->lines[i + 1], sizeof(ed->lines[i]));
  }
  --ed->line_count;
  if (ed->line_count == 0) { ed->line_count = 1; }
  ed->dirty = true;
}

void pico_insert_char(struct pico_editor *ed, char c) {
  if (ed->line_count == 0) { ed->line_count = 1; }
  if (c == '\t') { c = ' '; }
  char *line = ed->lines[ed->cursor_row];
  size_t len = strlen(line);
  if (len + 1 >= PICO_LINE_CAP) {
    pico_set_status(ed, "Line is full");
    return;
  }
  memmove(line + ed->cursor_col + 1, line + ed->cursor_col, len - ed->cursor_col + 1);
  line[ed->cursor_col++] = c;
  ed->dirty = true;
}

void pico_insert_newline(struct pico_editor *ed) {
  if (ed->line_count == 0) { ed->line_count = 1; }
  char tail[PICO_LINE_CAP];
  snprintf(tail, sizeof(tail), "%s", ed->lines[ed->cursor_row] + ed->cursor_col);
  ed->lines[ed->cursor_row][ed->cursor_col] = '\0';
  if (!pico_insert_empty_line(ed, ed->cursor_row + 1)) { return; }
  snprintf(ed->lines[ed->cursor_row + 1], sizeof(ed->lines[0]), "%s", tail);
  ++ed->cursor_row;
  ed->cursor_col = 0;
}

void pico_backspace(struct pico_editor *ed) {
  if (ed->line_count == 0) { ed->line_count = 1; }
  if (ed->cursor_col > 0) {
    char *line = ed->lines[ed->cursor_row];
    size_t len = strlen(line);
    memmove(line + ed->cursor_col - 1, line + ed->cursor_col, len - ed->cursor_col + 1);
    --ed->cursor_col;
    ed->dirty = true;
    return;
  }
  if (ed->cursor_row == 0) { return; }
  size_t prev_len = strlen(ed->lines[ed->cursor_row - 1]);
  size_t cur_len = strlen(ed->lines[ed->cursor_row]);
  if (prev_len + cur_len >= PICO_LINE_CAP) {
    pico_set_status(ed, "Joined line would be too long");
    return;
  }
  memcpy(ed->lines[ed->cursor_row - 1] + prev_len, ed->lines[ed->cursor_row], cur_len + 1);
  delete_line_at(ed, ed->cursor_row);
  --ed->cursor_row;
  ed->cursor_col = prev_len;
}

void pico_delete_char(struct pico_editor *ed) {
  if (ed->line_count == 0) { ed->line_count = 1; }
  char *line = ed->lines[ed->cursor_row];
  size_t len = strlen(line);
  if (ed->cursor_col < len) {
    memmove(line + ed->cursor_col, line + ed->cursor_col + 1, len - ed->cursor_col);
    ed->dirty = true;
    return;
  }
  if (ed->cursor_row + 1 >= ed->line_count) { return; }
  size_t next_len = strlen(ed->lines[ed->cursor_row + 1]);
  if (len + next_len >= PICO_LINE_CAP) {
    pico_set_status(ed, "Joined line would be too long");
    return;
  }
  memcpy(line + len, ed->lines[ed->cursor_row + 1], next_len + 1);
  delete_line_at(ed, ed->cursor_row + 1);
}

void pico_cut_line(struct pico_editor *ed) {
  if (ed->line_count == 0) { ed->line_count = 1; }
  snprintf(ed->cut_buf, sizeof(ed->cut_buf), "%s", ed->lines[ed->cursor_row]);
  delete_line_at(ed, ed->cursor_row);
  ed->cursor_col = 0;
  pico_set_status(ed, "Cut line");
}

void pico_uncut_line(struct pico_editor *ed) {
  if (ed->line_count == 0) { ed->line_count = 1; }
  if (ed->cut_buf[0] == '\0') {
    pico_set_status(ed, "Cut buffer is empty");
    return;
  }
  if (!pico_insert_empty_line(ed, ed->cursor_row)) { return; }
  snprintf(ed->lines[ed->cursor_row], sizeof(ed->lines[0]), "%s", ed->cut_buf);
  ed->cursor_col = 0;
  pico_set_status(ed, "Uncut line");
}

void pico_move_down(struct pico_editor *ed) {
  if (ed->cursor_row + 1 < ed->line_count) {
    ++ed->cursor_row;
    return;
  }
  if (pico_insert_empty_line(ed, ed->line_count)) { ++ed->cursor_row; }
}
