#include "run_modes.h"
#include "runner.h"

#include <stdio.h>
#include <string.h>

void print_guest_checks(const char *buf) {
  const char *p = buf;
  bool printed = false;
  while (*p != '\0') {
    const char *line_end = strchr(p, '\n');
    size_t len = line_end == NULL ? strlen(p) : (size_t)(line_end - p);
    char line[4096];
    size_t copy_len = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, p, copy_len);
    line[copy_len] = '\0';
    if (strstr(line, "[spore]") != NULL || strstr(line, "[cell ") != NULL ||
        strstr(line, "[kernel] lower sync fault") != NULL) {
      fwrite(p, 1, len, stdout);
      fputc('\n', stdout);
      printed = true;
    }
    if (line_end == NULL) { break; }
    p = line_end + 1;
  }
  if (!printed) { fputs(buf, stdout); }
}

bool answer_guest_stdin(int fd, const char *output) {
  if (!strstr(output, "[spore] stdin demo: child blocking on read(0)")) { return false; }
  write_serial_input(fd, "z\n", 2);
  return true;
}
