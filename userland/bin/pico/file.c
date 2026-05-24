#include "pico.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int pico_load_file(struct pico_editor *ed, const char *path) {
  snprintf(ed->path, sizeof(ed->path), "%s", path);
  ed->line_count = 0;
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      ed->line_count = 1;
      ed->lines[0][0] = '\0';
      ed->dirty = false;
      return 0;
    }
    perror(path);
    return 1;
  }

  ssize_t total = 0;
  for (;;) {
    ssize_t n = read(fd, ed->file_buf + total, sizeof(ed->file_buf) - 1 - (size_t)total);
    if (n < 0) {
      perror("read");
      close(fd);
      return 1;
    }
    if (n == 0) { break; }
    total += n;
    if ((size_t)total + 1 >= sizeof(ed->file_buf)) { break; }
  }
  close(fd);
  ed->file_buf[total] = '\0';

  char *p = ed->file_buf;
  while (*p != '\0' && ed->line_count < PICO_MAX_LINES) {
    char *end = strchr(p, '\n');
    if (end != NULL) { *end = '\0'; }
    snprintf(ed->lines[ed->line_count++], sizeof(ed->lines[0]), "%s", p);
    if (end == NULL) { break; }
    p = end + 1;
  }
  if (ed->line_count == 0) { ed->line_count = 1; }
  ed->dirty = false;
  return 0;
}

int pico_save_file(struct pico_editor *ed) {
  int fd = open(ed->path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
  if (fd < 0) {
    pico_set_status(ed, "Write failed: %s", strerror(errno));
    return 1;
  }
  for (size_t i = 0; i < ed->line_count; ++i) {
    size_t len = strlen(ed->lines[i]);
    if (write(fd, ed->lines[i], len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
      pico_set_status(ed, "Write failed: %s", strerror(errno));
      close(fd);
      return 1;
    }
  }
  close(fd);
  ed->dirty = false;
  pico_set_status(ed, "Wrote %u lines to %s", (unsigned)ed->line_count, ed->path);
  return 0;
}
