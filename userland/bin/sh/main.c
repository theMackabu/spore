#include "sh.h"

#include <stdlib.h>

int main(void) {
  setenv("PATH", "/bin:.", 0);
  setenv("HOME", "/", 0);
  setenv("USER", "root", 0);
  setenv("LOGNAME", "root", 0);
  setenv("SHELL", "/bin/sh", 0);

  char line[LINE_CAP];
  int last = 0;
  for (;;) {
    sh_reap_jobs(true);
    if (sh_read_line(line, sizeof(line)) < 0) { return last; }
    last = sh_execute_line(line, last);
  }
}
