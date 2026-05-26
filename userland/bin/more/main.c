#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

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

static int raw_getch(void) {
  struct termios old_term;
  if (tcgetattr(STDIN_FILENO, &old_term) != 0) { return getchar(); }
  struct termios raw = old_term;
  raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) { return getchar(); }
  int ch = getchar();
  (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_term);
  return ch;
}

static int prompt_more(void) {
  fputs("\033[7m--More--\033[0m", stdout);
  fflush(stdout);
  int ch = raw_getch();
  fputs("\r        \r", stdout);
  fflush(stdout);
  return ch;
}

static int more_stream(FILE *f, bool interactive) {
  char buf[512];
  int rows = terminal_rows();
  int page_lines = rows > 1 ? rows - 1 : 23;
  int lines = 0;

  while (fgets(buf, sizeof(buf), f) != NULL) {
    fputs(buf, stdout);
    if (strchr(buf, '\n') == NULL) { continue; }
    if (!interactive) { continue; }
    if (++lines < page_lines) { continue; }

    int ch = prompt_more();
    if (ch == 'q' || ch == 'Q') { return EXIT_SUCCESS; }
    lines = (ch == '\n' || ch == '\r') ? page_lines - 1 : 0;
  }

  return ferror(f) ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  bool input_is_terminal = stdin_is_terminal();
  bool interactive = isatty(STDOUT_FILENO) && input_is_terminal;
  if (argc == 2 && streq(argv[1], "--help")) { return usage("more", "[FILE...]"); }
  if (argc == 1 && input_is_terminal) {
    fputs("Missing filename (\"more --help\" for help)\n", stderr);
    return EXIT_FAILURE;
  }
  if (argc == 1) { return more_stream(stdin, interactive); }

  int rc = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    FILE *f = fopen(argv[i], "r");
    if (f == NULL) {
      perror(argv[i]);
      rc = EXIT_FAILURE;
      continue;
    }
    if (more_stream(f, interactive) != EXIT_SUCCESS) { rc = EXIT_FAILURE; }
    fclose(f);
  }
  return rc;
}
