#include <spore.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern char **environ;

static void exec_shell(void) {
  const char *shell = getenv("SHELL");
  if (shell == NULL || shell[0] == '\0') { shell = "/bin/msh"; }
  char *argv[] = {(char *)shell, NULL};
  execve(shell, argv, environ);
}

int main(int argc, char **argv) {
  if (argc < 2 || streq(argv[1], "--help")) { return usage("chroot", "NEWROOT [COMMAND [ARG]...]"); }
  if (chroot(argv[1]) != 0) {
    perror("chroot");
    return EXIT_FAILURE;
  }
  if (chdir("/") != 0) {
    perror("chdir");
    return EXIT_FAILURE;
  }
  if (argc == 2) {
    exec_shell();
    perror("chroot");
    return errno == ENOENT ? 127 : 126;
  }
  execvp(argv[2], &argv[2]);
  perror(argv[2]);
  return errno == ENOENT ? 127 : 126;
}
