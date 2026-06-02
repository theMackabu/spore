#include <stdio.h>
#include <string.h>

static int exec_private_data = 0x12345678;

int main(int argc, char **argv) {
  const char *arg = argc > 1 ? argv[1] : "missing";
  if (strcmp(arg, "poison-data") == 0) {
    exec_private_data = 0x5a5a5a5a;
    printf("[spore] exec child: poisoned private data\n");
    return 0;
  }
  if (strcmp(arg, "check-data") == 0) {
    int ok = exec_private_data == 0x12345678;
    printf("[spore] exec child: private data %s value=0x%x\n", ok ? "clean" : "dirty", exec_private_data);
    return ok ? 0 : 43;
  }
  printf("[spore] exec child: argv=%s\n", arg);
  return 42;
}
