#include <fcntl.h>
#include <spore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *elf_machine(uint16_t machine) {
  switch (machine) {
  case 183:
    return "ARM aarch64";
  case 62:
    return "x86-64";
  default:
    return "unknown machine";
  }
}

static void describe_elf(const unsigned char *hdr, ssize_t n) {
  if (n < 20) {
    puts("ELF file");
    return;
  }
  const char *bits = hdr[4] == 2 ? "64-bit" : (hdr[4] == 1 ? "32-bit" : "unknown-class");
  const char *endian = hdr[5] == 1 ? "LSB" : (hdr[5] == 2 ? "MSB" : "unknown-endian");
  uint16_t type = (uint16_t)hdr[16] | ((uint16_t)hdr[17] << 8);
  uint16_t machine = (uint16_t)hdr[18] | ((uint16_t)hdr[19] << 8);
  const char *kind = type == 2 ? "executable" : (type == 3 ? "pie executable/shared object" : "ELF file");
  printf("ELF %s %s %s, %s\n", bits, endian, kind, elf_machine(machine));
}

static int describe(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    perror(path);
    return EXIT_FAILURE;
  }
  printf("%s: ", path);
  if (S_ISDIR(st.st_mode)) {
    puts("directory");
    return EXIT_SUCCESS;
  }
  if (S_ISCHR(st.st_mode)) {
    puts("character special");
    return EXIT_SUCCESS;
  }
  if (!S_ISREG(st.st_mode)) {
    puts("special file");
    return EXIT_SUCCESS;
  }

  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror(path);
    return EXIT_FAILURE;
  }
  unsigned char hdr[64] = {0};
  ssize_t n = read(fd, hdr, sizeof(hdr));
  close(fd);
  if (n < 0) {
    perror(path);
    return EXIT_FAILURE;
  }
  if (n >= 4 && hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
    describe_elf(hdr, n);
  } else if (n >= 2 && hdr[0] == '#' && hdr[1] == '!') {
    puts("script text executable");
  } else if (st.st_size == 0) {
    puts("empty");
  } else {
    puts("data");
  }
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  if (argc < 2) { return usage("file", "PATH..."); }
  int rc = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    if (describe(argv[i]) != EXIT_SUCCESS) { rc = EXIT_FAILURE; }
  }
  return rc;
}
