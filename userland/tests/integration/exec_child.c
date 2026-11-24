#include <stdint.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <unistd.h>
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
  extern char _end[];
  uintptr_t end = (uintptr_t)_end;
  uintptr_t heap = (uintptr_t)sbrk(0);
  uintptr_t loader = getauxval(AT_BASE);
  if (heap < end || (loader > end && heap >= loader)) {
    printf("[spore] exec heap outside executable range\n");
    return 43;
  }
  printf("[spore] exec child: argv=%s\n", arg);
  return 42;
}
