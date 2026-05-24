#include "sh.h"

#include <stdlib.h>

int main(void) {
  setenv("PATH", "/bin:.", 0);
  setenv("HOME", "/", 0);

  char line[LINE_CAP];
  int last = 0;
  for (;;) {
    if (sh_read_line(line, sizeof(line)) < 0) { return last; }
    last = sh_execute_line(line, last);
  }
}
