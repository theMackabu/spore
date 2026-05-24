#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) { return usage("sudo", "COMMAND [ARG...]"); }

  struct user_entry self;
  if (!user_by_uid((unsigned)getuid(), &self)) {
    eprintf("sudo: current user is unknown\n");
    return EXIT_FAILURE;
  }

  bool nopasswd = false;
  if (geteuid() != 0 && !sudo_user_allowed(self.name, &nopasswd)) {
    eprintf("sudo: %s is not in the sudoers file\n", self.name);
    return EXIT_FAILURE;
  }

  if (geteuid() != 0 && !nopasswd) {
    char input[80];
    printf("[sudo] password for %s: ", self.name);
    fflush(stdout);
    if (fgets(input, sizeof(input), stdin) == NULL) { return EXIT_FAILURE; }
    input[strcspn(input, "\r\n")] = '\0';
    if (!password_matches(self.name, input)) {
      eprintf("sudo: authentication failed\n");
      return EXIT_FAILURE;
    }
  }

  if (setgid(0) != 0 || setuid(0) != 0) {
    perror("sudo");
    return EXIT_FAILURE;
  }

  execvp(argv[1], &argv[1]);
  perror(argv[1]);
  return 127;
}
