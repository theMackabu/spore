#include <stdlib.h>

#include <stdio.h>
#include <unistd.h>

int main(void) {
  char cwd[128];
  if (getcwd(cwd, sizeof(cwd)) == NULL) { return EXIT_FAILURE; }
  puts(cwd);
  return EXIT_SUCCESS;
}
