#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc < 2) { return usage("sudo", "COMMAND [ARG...]"); }

  uid_t real_uid = getuid();
  struct user_entry self;
  if (!user_by_uid((unsigned)real_uid, &self)) {
    eprintf("sudo: current user is unknown\n");
    return EXIT_FAILURE;
  }

  bool nopasswd = false;
  if (real_uid != 0 && !sudo_user_allowed(self.name, &nopasswd)) {
    eprintf("sudo: %s is not in the sudoers file\n", self.name);
    return EXIT_FAILURE;
  }

  if (real_uid != 0 && !nopasswd) {
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
