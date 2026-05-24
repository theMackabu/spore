#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  const char *name = argc > 1 ? argv[1] : NULL;
  struct user_entry self;
  if (name == NULL) {
    if (!user_by_uid((unsigned)getuid(), &self)) {
      eprintf("passwd: current user is unknown\n");
      return EXIT_FAILURE;
    }
    name = self.name;
  }
  if (argc > 2) { return usage("passwd", "[USER]"); }
  if (getuid() != 0) {
    if (!user_by_uid((unsigned)getuid(), &self) || !streq(self.name, name)) {
      eprintf("passwd: permission denied\n");
      return EXIT_FAILURE;
    }
  }
  struct user_entry target;
  if (!user_by_name(name, &target)) {
    eprintf("passwd: unknown user: %s\n", name);
    return EXIT_FAILURE;
  }
  char p1[80];
  char p2[80];
  printf("New password: ");
  fflush(stdout);
  if (fgets(p1, sizeof(p1), stdin) == NULL) { return EXIT_FAILURE; }
  printf("Retype password: ");
  fflush(stdout);
  if (fgets(p2, sizeof(p2), stdin) == NULL) { return EXIT_FAILURE; }
  p1[strcspn(p1, "\r\n")] = '\0';
  p2[strcspn(p2, "\r\n")] = '\0';
  if (!streq(p1, p2)) {
    eprintf("passwd: passwords do not match\n");
    return EXIT_FAILURE;
  }
  if (!set_shadow_password(name, p1)) {
    perror("passwd");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
