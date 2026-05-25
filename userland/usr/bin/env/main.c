#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static void clear_env(void) {
  for (char **env = environ; env != NULL && *env != NULL; ++env) {
    char *eq = strchr(*env, '=');
    if (eq != NULL) {
      *eq = '\0';
      unsetenv(*env);
      *eq = '=';
      env = environ - 1;
    }
  }
}

int main(int argc, char **argv) {
  int i = 1;
  if (i < argc && streq(argv[i], "-i")) {
    clear_env();
    ++i;
  }
  for (; i < argc; ++i) {
    char *eq = strchr(argv[i], '=');
    if (eq == NULL) { break; }
    *eq = '\0';
    setenv(argv[i], eq + 1, 1);
    *eq = '=';
  }
  if (i == argc) {
    for (char **env = environ; env != NULL && *env != NULL; ++env) {
      puts(*env);
    }
    return EXIT_SUCCESS;
  }
  execvp(argv[i], &argv[i]);
  perror(argv[i]);
  return 127;
}
