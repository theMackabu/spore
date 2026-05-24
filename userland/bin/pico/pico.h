#pragma once

#include <stdbool.h>
#include <stddef.h>

enum {
  PICO_DEFAULT_ROWS = 38,
  PICO_DEFAULT_COLS = 72,
  PICO_MIN_ROWS = 8,
  PICO_MIN_COLS = 40,
  PICO_MAX_ROWS = 60,
  PICO_MAX_COLS = 200,
  PICO_MAX_LINES = 1024,
  PICO_LINE_CAP = 256,
  PICO_FILE_CAP = 262144,
  PICO_STATUS_CAP = 256,
  PICO_PATH_CAP = 256,
  PICO_TAB_WIDTH = 2,
  PICO_RIGHT_GUTTER = 8,
};

enum pico_key {
  PICO_KEY_NONE = 0,
  PICO_KEY_CTRL_G = 7,
  PICO_KEY_CTRL_K = 11,
  PICO_KEY_CTRL_O = 15,
  PICO_KEY_CTRL_U = 21,
  PICO_KEY_CTRL_X = 24,
  PICO_KEY_BACKSPACE = 127,
  PICO_KEY_UP = 1000,
  PICO_KEY_DOWN,
  PICO_KEY_LEFT,
  PICO_KEY_RIGHT,
  PICO_KEY_HOME,
  PICO_KEY_END,
  PICO_KEY_DELETE,
};

struct pico_editor {
  char path[PICO_PATH_CAP];
  char lines[PICO_MAX_LINES][PICO_LINE_CAP];
  char file_buf[PICO_FILE_CAP];
  char cut_buf[PICO_LINE_CAP];
  char status[PICO_STATUS_CAP];
  size_t line_count;
  size_t cursor_row;
  size_t cursor_col;
  size_t row_offset;
  size_t screen_rows;
  size_t screen_cols;
  bool dirty;
  bool running;
  bool exit_confirm;
};

size_t pico_text_rows(const struct pico_editor *ed);
size_t pico_text_cols(const struct pico_editor *ed);
void pico_set_status(struct pico_editor *ed, const char *fmt, ...);
void pico_init_terminal(struct pico_editor *ed);
void pico_restore_terminal(void);
int pico_read_key(void);
void pico_clear_screen(void);

void pico_clamp_cursor(struct pico_editor *ed);
size_t pico_line_len(const struct pico_editor *ed, size_t row);
bool pico_insert_empty_line(struct pico_editor *ed, size_t row);
void pico_insert_char(struct pico_editor *ed, char c);
void pico_insert_newline(struct pico_editor *ed);
void pico_backspace(struct pico_editor *ed);
void pico_delete_char(struct pico_editor *ed);
void pico_cut_line(struct pico_editor *ed);
void pico_uncut_line(struct pico_editor *ed);
void pico_move_down(struct pico_editor *ed);

int pico_load_file(struct pico_editor *ed, const char *path);
int pico_save_file(struct pico_editor *ed);
void pico_redraw(struct pico_editor *ed);
void pico_show_help(struct pico_editor *ed);
