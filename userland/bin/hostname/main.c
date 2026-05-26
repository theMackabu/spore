#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>

#ifndef SYS_uname
#define SYS_uname 160
#endif
#ifndef SYS_sethostname
#define SYS_sethostname 161
#endif

struct spore_utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

static int set_name(const char *name) {
  if (syscall(SYS_sethostname, name, strlen(name)) != 0) {
    perror("hostname");
    return EXIT_FAILURE;
  }
  FILE *f = fopen("/etc/hostname", "w");
  if (f != NULL) {
    fprintf(f, "%s\n", name);
    fclose(f);
  }
  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "-F") == 0) {
    FILE *f = fopen(argv[2], "r");
    if (f == NULL) {
      perror(argv[2]);
      return EXIT_FAILURE;
    }
    char name[64];
    if (fgets(name, sizeof(name), f) == NULL) {
      fclose(f);
      return EXIT_FAILURE;
    }
    fclose(f);
    name[strcspn(name, "\r\n")] = '\0';
    return set_name(name);
  }
  if (argc > 2) {
    fputs("usage: hostname [-F FILE]|[NAME]\n", stderr);
    return EXIT_FAILURE;
  }
  if (argc == 2) {
    return set_name(argv[1]);
  }
  struct spore_utsname u;
  if (syscall(SYS_uname, &u) != 0) {
    perror("hostname");
    return EXIT_FAILURE;
  }
  puts(u.nodename);
  return EXIT_SUCCESS;
}
