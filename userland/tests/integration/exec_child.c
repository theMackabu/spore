#include <stdio.h>

int main(int argc, char **argv) {
  const char *arg = argc > 1 ? argv[1] : "missing";
  printf("[spore] exec child: argv=%s\n", arg);
  return 42;
}
