#include "msh.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
  char line[LINE_CAP];
  int last = 0;
  last = sh_source_file("/etc/profile", last, false);

  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') { home = "/"; }
  (void)chdir(home);
  char cwd[128];
  if (getcwd(cwd, sizeof(cwd)) != NULL) { setenv("PWD", cwd, 1); }

  char startup[256];
  if (snprintf(startup, sizeof(startup), "%s/.profile", home) > 0) { last = sh_source_file(startup, last, false); }
  if (snprintf(startup, sizeof(startup), "%s/.mshrc", home) > 0) { last = sh_source_file(startup, last, false); }

  for (;;) {
    sh_reap_jobs(true);
    if (sh_read_line(line, sizeof(line)) < 0) { return last; }
    last = sh_execute_line(line, last);
  }
}
