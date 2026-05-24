#include "msh.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void make_prompt(char *out, size_t cap) {
  char cwd[128];
  snprintf(out, cap, "%s $ ", getcwd(cwd, sizeof(cwd)) == NULL ? "/" : cwd);
}

int sh_read_line(char *buf, size_t cap) {
  char prompt[160];
  make_prompt(prompt, sizeof(prompt));
  fputs(prompt, stdout);
  fflush(stdout);
  if (fgets(buf, (int)cap, stdin) == NULL) { return -1; }
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
    buf[--len] = '\0';
  }
  return 0;
}
