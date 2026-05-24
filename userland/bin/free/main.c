#include <spore.h>

#include <stdio.h>
#include <stdlib.h>

static int read_meminfo(unsigned long long *total_kib, unsigned long long *used_kib, unsigned long long *free_kib) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL) {
    perror("/proc/meminfo");
    return EXIT_FAILURE;
  }
  char line[128];
  *total_kib = 0;
  *used_kib = 0;
  *free_kib = 0;
  while (fgets(line, sizeof(line), f) != NULL) {
    char key[64];
    unsigned long long value = 0;
    if (sscanf(line, "%63[^:]: %llu", key, &value) != 2) { continue; }
    if (streq(key, "MemTotalKiB")) {
      *total_kib = value;
    } else if (streq(key, "MemUsedKiB")) {
      *used_kib = value;
    } else if (streq(key, "MemFreeKiB")) {
      *free_kib = value;
    }
  }
  fclose(f);
  return *total_kib == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  bool human = false;
  if (argc > 2 || (argc == 2 && !streq(argv[1], "-h") && !streq(argv[1], "--human"))) { return usage("free", "[-h]"); }
  if (argc == 2) { human = true; }

  unsigned long long total = 0;
  unsigned long long used = 0;
  unsigned long long free_kib = 0;
  if (read_meminfo(&total, &used, &free_kib) != EXIT_SUCCESS) { return EXIT_FAILURE; }
  if (used == 0 && total > free_kib) { used = total - free_kib; }
  if (human) {
    printf("        total  used  free\n");
    printf("Mem:    %lluM  %lluM  %lluM\n", total / 1024, used / 1024, free_kib / 1024);
  } else {
    printf("              total        used        free\n");
    printf("Mem:     %10llu  %10llu  %10llu\n", total, used, free_kib);
  }
  return EXIT_SUCCESS;
}
