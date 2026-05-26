#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

struct line_vec {
  char **items;
  size_t len;
  size_t cap;
};

static void free_lines(struct line_vec *v) {
  for (size_t i = 0; i < v->len; ++i) {
    free(v->items[i]);
  }
  free(v->items);
}

static int push_line(struct line_vec *v, const char *line) {
  if (v->len == v->cap) {
    size_t next = v->cap == 0 ? 64 : v->cap * 2;
    char **items = realloc(v->items, next * sizeof(*items));
    if (items == NULL) { return -1; }
    v->items = items;
    v->cap = next;
  }
  v->items[v->len] = strdup(line);
  if (v->items[v->len] == NULL) { return -1; }
  ++v->len;
  return 0;
}

static int load_stream(FILE *f, struct line_vec *out) {
  char buf[512];
  while (fgets(buf, sizeof(buf), f) != NULL) {
    if (push_line(out, buf) != 0) { return -1; }
  }
  return ferror(f) ? -1 : 0;
}

static bool stdin_is_terminal(void) {
  struct stat st;
  if (fstat(STDIN_FILENO, &st) == 0) { return S_ISCHR(st.st_mode); }
  return isatty(STDIN_FILENO);
}

static int terminal_rows(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2) { return ws.ws_row; }
  return 24;
}

static void clear_screen(void) {
  fputs("\033[H\033[2J", stdout);
}

static int read_key(void) {
  int ch = getchar();
  if (ch != '\033') { return ch; }
  int a = getchar();
  if (a != '[') { return ch; }
  int b = getchar();
  if (b == 'A') { return 'k'; }
  if (b == 'B') { return 'j'; }
  return ch;
}

static void draw_page(const struct line_vec *lines, const char *name, size_t top) {
  int rows = terminal_rows();
  int body = rows > 1 ? rows - 1 : 23;
  clear_screen();
  for (int i = 0; i < body; ++i) {
    size_t idx = top + (size_t)i;
    if (idx < lines->len) {
      fputs(lines->items[idx], stdout);
      if (strchr(lines->items[idx], '\n') == NULL) { putchar('\n'); }
    } else {
      puts("~");
    }
  }
  printf("\033[7m%s lines %zu-%zu/%zu (q quit, space next, b prev)\033[0m", name, lines->len == 0 ? 0 : top + 1,
         top + (size_t)body < lines->len ? top + (size_t)body : lines->len, lines->len);
  fflush(stdout);
}

static int browse(struct line_vec *lines, const char *name) {
  if (!stdin_is_terminal() || !isatty(STDOUT_FILENO)) {
    for (size_t i = 0; i < lines->len; ++i) {
      fputs(lines->items[i], stdout);
    }
    return EXIT_SUCCESS;
  }

  struct termios old_term;
  if (tcgetattr(STDIN_FILENO, &old_term) != 0) { return EXIT_FAILURE; }
  struct termios raw = old_term;
  raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) { return EXIT_FAILURE; }

  size_t top = 0;
  for (;;) {
    int rows = terminal_rows();
    int body = rows > 1 ? rows - 1 : 23;
    draw_page(lines, name, top);
    int ch = read_key();
    if (ch == 'q' || ch == 'Q') { break; }
    if (ch == ' ' || ch == 'f') {
      size_t step = (size_t)body;
      top = top + step < lines->len ? top + step : top;
    } else if (ch == 'b') {
      size_t step = (size_t)body;
      top = top > step ? top - step : 0;
    } else if (ch == '\n' || ch == '\r' || ch == 'j') {
      if (top + 1 < lines->len) { ++top; }
    } else if (ch == 'k') {
      if (top > 0) { --top; }
    }
  }

  (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
  clear_screen();
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  if (argc == 2 && streq(argv[1], "--help")) { return usage("less", "[FILE]"); }
  if (argc > 2) { return usage("less", "[FILE]"); }
  if (argc == 1 && stdin_is_terminal()) {
    fputs("Missing filename (\"less --help\" for help)\n", stderr);
    return EXIT_FAILURE;
  }

  struct line_vec lines = {0};
  const char *name = argc == 2 ? argv[1] : "stdin";
  bool from_stdin = argc == 1;
  int rc = EXIT_SUCCESS;

  if (argc == 2) {
    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
      perror(argv[1]);
      return EXIT_FAILURE;
    }
    if (load_stream(f, &lines) != 0) { rc = EXIT_FAILURE; }
    fclose(f);
  } else if (load_stream(stdin, &lines) != 0) {
    rc = EXIT_FAILURE;
  }

  if (rc == EXIT_SUCCESS && from_stdin) {
    for (size_t i = 0; i < lines.len; ++i) {
      fputs(lines.items[i], stdout);
    }
  } else if (rc == EXIT_SUCCESS) {
    rc = browse(&lines, name);
  }
  free_lines(&lines);
  return rc;
}
