#include "pico.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved_termios;
static bool have_saved_termios;

static size_t bounded_size(long value, size_t min, size_t max, size_t fallback) {
  if (value < (long)min) { return fallback; }
  if (value > (long)max) { return max; }
  return (size_t)value;
}

static void apply_env_size(struct pico_editor *ed) {
  const char *lines = getenv("LINES");
  const char *cols = getenv("COLUMNS");
  if (lines != NULL && lines[0] != '\0') {
    ed->screen_rows = bounded_size(strtol(lines, NULL, 10), PICO_MIN_ROWS, PICO_MAX_ROWS, ed->screen_rows);
  }
  if (cols != NULL && cols[0] != '\0') {
    ed->screen_cols = bounded_size(strtol(cols, NULL, 10), PICO_MIN_COLS, PICO_MAX_COLS, ed->screen_cols);
  }
}

void pico_init_terminal(struct pico_editor *ed) {
  ed->screen_rows = PICO_DEFAULT_ROWS;
  ed->screen_cols = PICO_DEFAULT_COLS;

  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (ws.ws_row >= PICO_MIN_ROWS) { ed->screen_rows = ws.ws_row > PICO_MAX_ROWS ? PICO_MAX_ROWS : ws.ws_row; }
    if (ws.ws_col >= PICO_MIN_COLS) { ed->screen_cols = ws.ws_col > PICO_MAX_COLS ? PICO_MAX_COLS : ws.ws_col; }
  }
  apply_env_size(ed);

  if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
    struct termios raw = saved_termios;
    have_saved_termios = true;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  }
  apply_env_size(ed);
  static const char enter_alt[] = "\033[?1049h\033[?25l\033[2J\033[H";
  (void)write(STDOUT_FILENO, enter_alt, sizeof(enter_alt) - 1);
}

void pico_restore_terminal(void) {
  static const char leave_alt[] = "\033[?25h\033[?1049l";
  (void)write(STDOUT_FILENO, leave_alt, sizeof(leave_alt) - 1);
  if (have_saved_termios) { (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios); }
}

int pico_read_key(void) {
  char c;
  if (read(STDIN_FILENO, &c, 1) <= 0) { return PICO_KEY_NONE; }
  if (c == '\r') { return '\n'; }
  if (c != 27) { return (unsigned char)c; }

  char seq[3];
  if (read(STDIN_FILENO, &seq[0], 1) <= 0) { return PICO_KEY_NONE; }
  if (read(STDIN_FILENO, &seq[1], 1) <= 0) { return PICO_KEY_NONE; }
  if (seq[0] != '[') { return PICO_KEY_NONE; }
  if (seq[1] >= '0' && seq[1] <= '9') {
    if (read(STDIN_FILENO, &seq[2], 1) <= 0) { return PICO_KEY_NONE; }
    if (seq[2] == '~') {
      if (seq[1] == '1' || seq[1] == '7') { return PICO_KEY_HOME; }
      if (seq[1] == '3') { return PICO_KEY_DELETE; }
      if (seq[1] == '4' || seq[1] == '8') { return PICO_KEY_END; }
    }
    return PICO_KEY_NONE;
  }
  if (seq[1] == 'A') { return PICO_KEY_UP; }
  if (seq[1] == 'B') { return PICO_KEY_DOWN; }
  if (seq[1] == 'C') { return PICO_KEY_RIGHT; }
  if (seq[1] == 'D') { return PICO_KEY_LEFT; }
  if (seq[1] == 'H') { return PICO_KEY_HOME; }
  if (seq[1] == 'F') { return PICO_KEY_END; }
  return PICO_KEY_NONE;
}

void pico_clear_screen(void) {
  (void)write(STDOUT_FILENO, "\033[2J\033[H", 7);
}
