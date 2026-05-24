#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

enum uname_field {
  FIELD_SYSNAME = 1u << 0,
  FIELD_NODENAME = 1u << 1,
  FIELD_RELEASE = 1u << 2,
  FIELD_VERSION = 1u << 3,
  FIELD_MACHINE = 1u << 4,
  FIELD_PROCESSOR = 1u << 5,
  FIELD_PLATFORM = 1u << 6,
  FIELD_OS = 1u << 7,
};

static void print_help(void) {
  puts("usage: uname [OPTION]...");
  puts("  -a, --all");
  puts("  -s, --kernel-name");
  puts("  -n, --nodename");
  puts("  -r, --kernel-release");
  puts("  -v, --kernel-version");
  puts("  -m, --machine");
  puts("  -p, --processor");
  puts("  -i, --hardware-platform");
  puts("  -o, --operating-system");
  puts("      --help");
  puts("      --version");
}

static int add_long_option(const char *arg, unsigned *fields) {
  if (streq(arg, "--all")) {
    *fields |=
      FIELD_SYSNAME | FIELD_NODENAME | FIELD_RELEASE | FIELD_VERSION | FIELD_MACHINE | FIELD_PROCESSOR | FIELD_OS;
  } else if (streq(arg, "--kernel-name")) {
    *fields |= FIELD_SYSNAME;
  } else if (streq(arg, "--nodename")) {
    *fields |= FIELD_NODENAME;
  } else if (streq(arg, "--kernel-release")) {
    *fields |= FIELD_RELEASE;
  } else if (streq(arg, "--kernel-version")) {
    *fields |= FIELD_VERSION;
  } else if (streq(arg, "--machine")) {
    *fields |= FIELD_MACHINE;
  } else if (streq(arg, "--processor")) {
    *fields |= FIELD_PROCESSOR;
  } else if (streq(arg, "--hardware-platform")) {
    *fields |= FIELD_PLATFORM;
  } else if (streq(arg, "--operating-system")) {
    *fields |= FIELD_OS;
  } else {
    return -1;
  }
  return 0;
}

static int add_short_options(const char *arg, unsigned *fields) {
  for (const char *p = arg + 1; *p != '\0'; ++p) {
    switch (*p) {
    case 'a':
      *fields |=
        FIELD_SYSNAME | FIELD_NODENAME | FIELD_RELEASE | FIELD_VERSION | FIELD_MACHINE | FIELD_PROCESSOR | FIELD_OS;
      break;
    case 's':
      *fields |= FIELD_SYSNAME;
      break;
    case 'n':
      *fields |= FIELD_NODENAME;
      break;
    case 'r':
      *fields |= FIELD_RELEASE;
      break;
    case 'v':
      *fields |= FIELD_VERSION;
      break;
    case 'm':
      *fields |= FIELD_MACHINE;
      break;
    case 'p':
      *fields |= FIELD_PROCESSOR;
      break;
    case 'i':
      *fields |= FIELD_PLATFORM;
      break;
    case 'o':
      *fields |= FIELD_OS;
      break;
    default:
      return -1;
    }
  }
  return 0;
}

static void print_field(int *printed, const char *value) {
  if (*printed) { putchar(' '); }
  fputs(value, stdout);
  *printed = 1;
}

int main(int argc, char **argv) {
  unsigned fields = 0;
  for (int i = 1; i < argc; ++i) {
    if (streq(argv[i], "--help")) {
      print_help();
      return EXIT_SUCCESS;
    }
    if (streq(argv[i], "--version")) {
      puts("uname (Spore)");
      return EXIT_SUCCESS;
    }
    if (argv[i][0] != '-' || argv[i][1] == '\0') {
      return usage("uname", "[-a|-s|-n|-r|-v|-m|-p|-i|-o|--help|--version]...");
    }
    int rc = argv[i][1] == '-' ? add_long_option(argv[i], &fields) : add_short_options(argv[i], &fields);
    if (rc != 0) { return usage("uname", "[-a|-s|-n|-r|-v|-m|-p|-i|-o|--help|--version]..."); }
  }
  if (fields == 0) { fields = FIELD_SYSNAME; }

  struct utsname u;
  if (uname(&u) != 0) {
    perror("uname");
    return EXIT_FAILURE;
  }

  int printed = 0;
  if (fields & FIELD_SYSNAME) { print_field(&printed, u.sysname); }
  if (fields & FIELD_NODENAME) { print_field(&printed, u.nodename); }
  if (fields & FIELD_RELEASE) { print_field(&printed, u.release); }
  if (fields & FIELD_VERSION) { print_field(&printed, u.version); }
  if (fields & FIELD_MACHINE) { print_field(&printed, u.machine); }
  if (fields & FIELD_PROCESSOR) { print_field(&printed, u.machine); }
  if (fields & FIELD_PLATFORM) { print_field(&printed, u.machine); }
  if (fields & FIELD_OS) { print_field(&printed, u.domainname); }

  putchar('\n');
  return EXIT_SUCCESS;
}
