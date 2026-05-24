#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *name = argc > 1 ? argv[1] : "root";
  if (argc > 2) { return usage("su", "[USER]"); }
  struct user_entry user;
  if (!user_by_name(name, &user)) {
    eprintf("su: unknown user: %s\n", name);
    return EXIT_FAILURE;
  }
  if (getuid() != 0) {
    char input[80];
    printf("Password: ");
    fflush(stdout);
    if (fgets(input, sizeof(input), stdin) == NULL) { return EXIT_FAILURE; }
    input[strcspn(input, "\r\n")] = '\0';
    if (!password_matches(user.name, input)) {
      eprintf("su: authentication failed\n");
      return EXIT_FAILURE;
    }
  }
  if (setgid((gid_t)user.gid) != 0 || setuid((uid_t)user.uid) != 0) {
    perror("su");
    return EXIT_FAILURE;
  }
  setenv("HOME", user.home, 1);
  setenv("USER", user.name, 1);
  setenv("LOGNAME", user.name, 1);
  setenv("SHELL", user.shell, 1);
  setenv("PWD", user.home, 1);
  (void)chdir(user.home);
  execl(user.shell, basename(user.shell), (char *)NULL);
  perror(user.shell);
  return 127;
}
