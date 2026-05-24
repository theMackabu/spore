#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
  struct group_entry group;
  if (group_by_gid((unsigned)getgid(), &group)) {
    puts(group.name);
  } else {
    printf("%u\n", (unsigned)getgid());
  }
  return EXIT_SUCCESS;
}
