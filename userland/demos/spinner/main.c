#include <stdlib.h>

int main(void) {
  volatile unsigned long x = 0;
  for (;;) {
    ++x;
  }
  return EXIT_SUCCESS;
}
