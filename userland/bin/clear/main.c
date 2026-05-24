#include "util.h"

#include <stdio.h>

int main(void) {
  fputs("\033[2J\033[H", stdout);
  return SPORE_OK;
}
