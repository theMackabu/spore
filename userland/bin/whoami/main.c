#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
  struct user_entry user;
  if (user_by_uid((unsigned)getuid(), &user)) {
    puts(user.name);
  } else {
    printf("%u\n", (unsigned)getuid());
  }
  return EXIT_SUCCESS;
}
