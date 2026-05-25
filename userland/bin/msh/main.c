#include "msh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void update_pwd(void) {
  char cwd[128];
  if (getcwd(cwd, sizeof(cwd)) != NULL) { setenv("PWD", cwd, 1); }
}

int main(int argc, char **argv) {
  if (argc > 1) {
    if (streq(argv[1], "-c")) {
      if (argc < 3) { return usage("msh", "-c COMMAND"); }
      char line[LINE_CAP];
      snprintf(line, sizeof(line), "%s", argv[2]);
      return sh_execute_line(line, 0);
    }
    return sh_source_file(argv[1], 0, true);
  }

  char line[LINE_CAP];
  int last = 0;
  last = sh_source_file("/etc/profile", last, false);

  const char *home = getenv("HOME");
  if (home == NULL || home[0] == '\0') { home = "/"; }
  (void)chdir(home);
  update_pwd();
  sh_history_load();

  char startup[256];
  if (snprintf(startup, sizeof(startup), "%s/.profile", home) > 0) { last = sh_source_file(startup, last, false); }
  if (snprintf(startup, sizeof(startup), "%s/.mshrc", home) > 0) { last = sh_source_file(startup, last, false); }

  for (;;) {
    sh_reap_jobs(true);
    if (sh_read_line(line, sizeof(line)) < 0) { return last; }
    last = sh_execute_line(line, last);
  }
}
