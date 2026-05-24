#include <errno.h>
#include <fcntl.h>
#include <spore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
  MAX_LINES = 512,
  LINE_LEN = 256,
  FILE_BUF = 65536,
};

static char lines[MAX_LINES][LINE_LEN];
static char file_buf[FILE_BUF];
static size_t line_count;
static bool dirty;

static void chomp(char *s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[--len] = '\0';
  }
}

static bool insert_line(size_t index, const char *text) {
  if (line_count >= MAX_LINES || index > line_count) { return false; }
  for (size_t i = line_count; i > index; --i) {
    snprintf(lines[i], sizeof(lines[i]), "%s", lines[i - 1]);
  }
  snprintf(lines[index], sizeof(lines[index]), "%s", text);
  ++line_count;
  dirty = true;
  return true;
}

static bool delete_line(size_t index) {
  if (index >= line_count) { return false; }
  for (size_t i = index; i + 1 < line_count; ++i) {
    snprintf(lines[i], sizeof(lines[i]), "%s", lines[i + 1]);
  }
  --line_count;
  dirty = true;
  return true;
}

static void print_range(size_t first, size_t last) {
  if (line_count == 0) {
    puts("(empty)");
    return;
  }
  if (first >= line_count) { first = line_count - 1; }
  if (last >= line_count) { last = line_count - 1; }
  for (size_t i = first; i <= last; ++i) {
    printf("%3u  %s\n", (unsigned)(i + 1), lines[i]);
  }
}

static void help(void) {
  puts("commands:");
  puts("  p [n [m]]   print lines");
  puts("  a           append lines; finish with a single '.'");
  puts("  i n         insert before line n; finish with '.'");
  puts("  d n         delete line n");
  puts("  w           write file");
  puts("  q           quit if clean");
  puts("  q!          quit without writing");
  puts("  h           help");
}

static int read_line(char *buf, size_t cap, const char *prompt) {
  if (cap == 0) { return -1; }
  fputs(prompt, stdout);
  fflush(stdout);

  size_t len = 0;
  for (;;) {
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) { return -1; }
    if (c == '\r') { c = '\n'; }
    if (c == '\n') {
      putchar('\n');
      buf[len] = '\0';
      return 0;
    }
    if (c == 3) {
      puts("^C");
      buf[0] = '\0';
      return 0;
    }
    if (c == '\b' || c == 0x7f) {
      if (len > 0) {
        --len;
        fputs("\b \b", stdout);
        fflush(stdout);
      }
      continue;
    }
    if ((unsigned char)c < 0x20) { continue; }
    if (len + 1 < cap) {
      buf[len++] = c;
      (void)write(STDOUT_FILENO, &c, 1);
    }
  }
}

static void read_insert(size_t index) {
  char buf[LINE_LEN];
  for (;;) {
    if (read_line(buf, sizeof(buf), ". ") != 0) { return; }
    if (strcmp(buf, ".") == 0) { return; }
    if (!insert_line(index, buf)) {
      puts("edit: too many lines");
      return;
    }
    ++index;
  }
}

static int load_file(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) { return 0; }
    perror(path);
    return 1;
  }
  ssize_t total = 0;
  for (;;) {
    ssize_t n = read(fd, file_buf + total, sizeof(file_buf) - 1 - (size_t)total);
    if (n < 0) {
      perror("read");
      close(fd);
      return 1;
    }
    if (n == 0) { break; }
    total += n;
    if ((size_t)total + 1 >= sizeof(file_buf)) { break; }
  }
  close(fd);
  file_buf[total] = '\0';

  char *p = file_buf;
  while (*p != '\0' && line_count < MAX_LINES) {
    char *end = strchr(p, '\n');
    if (end != NULL) { *end = '\0'; }
    snprintf(lines[line_count++], sizeof(lines[0]), "%s", p);
    if (end == NULL) { break; }
    p = end + 1;
  }
  dirty = false;
  return 0;
}

static int save_file(const char *path) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd < 0) {
    perror(path);
    return 1;
  }
  for (size_t i = 0; i < line_count; ++i) {
    size_t len = strlen(lines[i]);
    if (write(fd, lines[i], len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
      perror("write");
      close(fd);
      return 1;
    }
  }
  close(fd);
  dirty = false;
  printf("%s: %u lines written\n", path, (unsigned)line_count);
  return 0;
}

static size_t line_arg(char *s, size_t fallback) {
  while (*s == ' ' || *s == '\t') {
    ++s;
  }
  if (*s == '\0') { return fallback; }
  long n = strtol(s, NULL, 10);
  if (n <= 0) { return fallback; }
  return (size_t)n - 1;
}

int main(int argc, char **argv) {
  if (argc != 2) { return usage(argv[0], "FILE"); }
  const char *path = argv[1];
  if (load_file(path) != 0) { return EXIT_FAILURE; }

  printf("edit: %s (%u lines). h for help.\n", path, (unsigned)line_count);
  char cmd[LINE_LEN];
  for (;;) {
    if (read_line(cmd, sizeof(cmd), "edit> ") != 0) { return dirty ? EXIT_FAILURE : EXIT_SUCCESS; }
    if (cmd[0] == '\0') { continue; }
    if (strcmp(cmd, "h") == 0 || strcmp(cmd, "?") == 0) {
      help();
    } else if (cmd[0] == 'p') {
      size_t first = line_arg(cmd + 1, 0);
      char *second = cmd + 1;
      while (*second == ' ' || *second == '\t') {
        ++second;
      }
      while (*second != '\0' && *second != ' ' && *second != '\t') {
        ++second;
      }
      size_t last = line_arg(second, line_count == 0 ? 0 : line_count - 1);
      print_range(first, last);
    } else if (strcmp(cmd, "a") == 0) {
      read_insert(line_count);
    } else if (cmd[0] == 'i') {
      read_insert(line_arg(cmd + 1, 0));
    } else if (cmd[0] == 'd') {
      if (!delete_line(line_arg(cmd + 1, line_count))) { puts("edit: no such line"); }
    } else if (strcmp(cmd, "w") == 0) {
      (void)save_file(path);
    } else if (strcmp(cmd, "q") == 0) {
      if (dirty) {
        puts("edit: unsaved changes; use q! to discard");
      } else {
        return EXIT_SUCCESS;
      }
    } else if (strcmp(cmd, "q!") == 0) {
      return EXIT_SUCCESS;
    } else {
      puts("edit: unknown command");
    }
  }
}
