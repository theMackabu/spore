#include <stdio.h>
#include <stdlib.h>

static int dump(FILE *f, long limit) {
  unsigned char buf[16];
  unsigned long off = 0;
  for (;;) {
    size_t want = sizeof(buf);
    if (limit >= 0) {
      if (limit == 0) { break; }
      if ((long)want > limit) { want = (size_t)limit; }
    }
    size_t n = fread(buf, 1, want, f);
    if (n == 0) { break; }
    if (limit >= 0) { limit -= (long)n; }
    printf("%08lx: ", off);
    for (size_t i = 0; i < 16; ++i) {
      if (i < n) {
        printf("%02x", buf[i]);
      } else {
        fputs("  ", stdout);
      }
      if ((i & 1) == 1) { putchar(' '); }
    }
    putchar(' ');
    for (size_t i = 0; i < n; ++i) {
      putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
    }
    putchar('\n');
    off += (unsigned long)n;
  }
  return ferror(f) ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  long limit = -1;
  int first = 1;
  if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'l' && argv[1][2] == '\0') {
    limit = strtol(argv[2], NULL, 10);
    if (limit < 0) {
      fputs("usage: xxd [-l LEN] [FILE...]\n", stderr);
      return EXIT_FAILURE;
    }
    first = 3;
  }
  if (first == argc) { return dump(stdin, limit); }
  int rc = EXIT_SUCCESS;
  for (int i = first; i < argc; ++i) {
    FILE *f = fopen(argv[i], "rb");
    if (f == NULL) {
      perror(argv[i]);
      rc = EXIT_FAILURE;
      continue;
    }
    if (dump(f, limit) != EXIT_SUCCESS) { rc = EXIT_FAILURE; }
    fclose(f);
  }
  return rc;
}
