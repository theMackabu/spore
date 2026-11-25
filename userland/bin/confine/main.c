#include "spore.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *executable(const char *name) {
  if (strchr(name, '/')) { return realpath(name, NULL); }
  const char *path = getenv("PATH");
  char *dirs = strdup(path ? path : "/bin:/usr/bin");
  if (!dirs) { return NULL; }
  char *next = dirs, *dir, *result = NULL;
  int error = ENOENT;
  while ((dir = strsep(&next, ":"))) {
    char *candidate;
    if (asprintf(&candidate, "%s/%s", *dir ? dir : ".", name) < 0) {
      error = ENOMEM;
      break;
    }
    struct stat st;
    if (stat(candidate, &st) == 0) {
      if (S_ISREG(st.st_mode) && access(candidate, X_OK) == 0) {
        result = realpath(candidate, NULL);
      } else {
        error = EACCES;
      }
    }
    free(candidate);
    if (result) { break; }
  }
  free(dirs);
  if (!result) { errno = error; }
  return result;
}

int main(int argc, char **argv) {
  if (argc < 3) { return usage(argv[0], "PROFILE COMMAND [ARG...]"); }
  const char *profile = argv[1];
  char *command = argv[2];
  if (streq(profile, "fs:/tmp")) {
    command = executable(command);
    if (!command) {
      int code = errno == ENOENT ? 127 : 126;
      perror(argv[2]);
      return code;
    }
    char *rule;
    if (asprintf(&rule, "fs:/tmp;exec:%s", command) < 0) { return 126; }
    profile = rule;
  }
  if (syscall(SYS_SPORE_APPLY_POLICY, profile) < 0) {
    perror("confine: apply policy");
    return 126;
  }
  execvp(command, argv + 2);
  int code = errno == ENOENT ? 127 : 126;
  perror(argv[2]);
  return code;
}
