#include <spore.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    fputs("usage: nslookup HOST\n", stderr);
    return EXIT_USAGE;
  }
  uint32_t ip = 0;
  if (!resolve_ipv4(argv[1], &ip)) {
    fprintf(stderr, "nslookup: can't resolve %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  char ip_s[32];
  format_ipv4(ip, ip_s, sizeof(ip_s));
  printf("%s -> %s\n", argv[1], ip_s);
  return EXIT_SUCCESS;
}
