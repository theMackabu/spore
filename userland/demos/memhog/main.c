#include <stdlib.h>

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned long long read_total_kib(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL) { return 0; }

  char line[128];
  unsigned long long total_kib = 0;
  while (fgets(line, sizeof(line), f) != NULL) {
    char key[64];
    unsigned long long value = 0;
    if (sscanf(line, "%63[^:]: %llu", key, &value) != 2) { continue; }
    if (strcmp(key, "MemTotalKiB") == 0 || strcmp(key, "MemTotal") == 0) {
      total_kib = value;
      break;
    }
  }
  fclose(f);
  return total_kib;
}

static int expect_denied_mmap(void) {
  void *p = mmap(NULL, 8 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    puts("memhog: mmap past cap failed cleanly");
    return EXIT_SUCCESS;
  }
  munmap(p, 8 * 1024 * 1024);
  puts("memhog: unexpected mmap success");
  return EXIT_FAILURE;
}

static void touch_pages(unsigned char *mem, size_t len) {
  for (size_t off = 0; off < len; off += 4096) {
    mem[off] = (unsigned char)(off >> 12);
  }
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--expect-deny") == 0) { return expect_denied_mmap(); }

  unsigned long long total_kib = read_total_kib();
  if (total_kib == 0) {
    puts("memhog: failed to read /proc/meminfo");
    return EXIT_FAILURE;
  }

  size_t bytes = (size_t)(total_kib / 2) * 1024;
  bytes &= ~(size_t)4095;
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    printf("memhog: mmap failed for %llu MiB\n", (unsigned long long)(bytes / 1024 / 1024));
    return EXIT_FAILURE;
  }

  printf("memhog: pid=%d target=%llu MiB (50%% of RAM)\n", (int)getpid(), (unsigned long long)(bytes / 1024 / 1024));
  fflush(stdout);
  touch_pages(p, bytes);
  printf("memhog: holding %llu MiB; kill %d to release\n", (unsigned long long)(bytes / 1024 / 1024), (int)getpid());
  fflush(stdout);

  for (;;) {
    sleep(3600);
  }
}
