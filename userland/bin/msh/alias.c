#include "msh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct alias sh_aliases[ALIAS_CAP];

int sh_alias_index(const char *name) {
  for (int i = 0; i < ALIAS_CAP; ++i) {
    if (sh_aliases[i].name && streq(sh_aliases[i].name, name)) { return i; }
  }
  return -1;
}

void sh_print_alias(int index) {
  printf("alias %s='", sh_aliases[index].name);
  for (const char *p = sh_aliases[index].value; *p; ++p) {
    if (*p == '\'') {
      fputs("'\\''", stdout);
    } else {
      putchar(*p);
    }
  }
  puts("'");
}

static int define_alias(const char *assignment) {
  char *name = strdup(assignment);
  if (!name) {
    perror("alias");
    return 1;
  }
  char *value = strchr(name, '=');
  *value++ = '\0';
  if (name[0] == '\0' ||
      strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_!%,@+.-") != strlen(name)) {
    eprintf("alias: invalid name: %s\n", name);
    free(name);
    return 1;
  }
  int index = sh_alias_index(name);
  if (index < 0) {
    for (index = 0; index < ALIAS_CAP && sh_aliases[index].name; ++index) {}
    if (index == ALIAS_CAP) {
      eprintf("alias: too many aliases\n");
      free(name);
      return 1;
    }
  }
  free(sh_aliases[index].name);
  sh_aliases[index] = (struct alias){.name = name, .value = value};
  return 0;
}

int sh_alias(int argc, char **argv) {
  int first = argc > 1 && streq(argv[1], "--") ? 2 : 1;
  if (argc == first) {
    for (int i = 0; i < ALIAS_CAP; ++i) {
      if (sh_aliases[i].name) { sh_print_alias(i); }
    }
    return 0;
  }
  int status = 0;
  for (int i = first; i < argc; ++i) {
    if (strchr(argv[i], '=')) {
      if (define_alias(argv[i]) != 0) { status = 1; }
    } else {
      int index = sh_alias_index(argv[i]);
      if (index >= 0) {
        sh_print_alias(index);
      } else {
        eprintf("alias: %s: not found\n", argv[i]);
        status = 1;
      }
    }
  }
  return status;
}

static void remove_alias(int index) {
  free(sh_aliases[index].name);
  sh_aliases[index] = (struct alias){0};
}

int sh_unalias(int argc, char **argv) {
  if (argc == 2 && streq(argv[1], "-a")) {
    for (int i = 0; i < ALIAS_CAP; ++i) {
      remove_alias(i);
    }
    return 0;
  }
  int first = argc > 1 && streq(argv[1], "--") ? 2 : 1;
  if (argc == first) { return usage("unalias", "NAME... | -a"); }
  int status = 0;
  for (int i = first; i < argc; ++i) {
    int index = sh_alias_index(argv[i]);
    if (index >= 0) {
      remove_alias(index);
    } else {
      eprintf("unalias: %s: not found\n", argv[i]);
      status = 1;
    }
  }
  return status;
}
