#include "util.h"

#include <stdio.h>
#include <unistd.h>

int main(void) {
  char cwd[128];
  if (getcwd(cwd, sizeof(cwd)) == NULL) { return SPORE_ERROR; }
  puts(cwd);
  return SPORE_OK;
}
