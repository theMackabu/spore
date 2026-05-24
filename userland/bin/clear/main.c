#include <stdio.h>
#include <stdlib.h>

int main(void) {
  fputs("\033[H\033[2J\033[3J", stdout);
  return EXIT_SUCCESS;
}
