#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
  bool user = false;
  bool group = false;
  bool groups = false;
  bool name = false;
  for (int i = 1; i < argc; ++i) {
    if (streq(argv[i], "-u")) {
      user = true;
    } else if (streq(argv[i], "-g")) {
      group = true;
    } else if (streq(argv[i], "-G")) {
      groups = true;
    } else if (streq(argv[i], "-n")) {
      name = true;
    } else {
      return usage("id", "[-u|-g|-G] [-n]");
    }
  }
  if (name) {
    if (user) {
      struct user_entry u;
      if (user_by_uid((unsigned)getuid(), &u)) {
        puts(u.name);
      } else {
        printf("%u\n", (unsigned)getuid());
      }
    } else {
      struct group_entry g;
      if (group_by_gid((unsigned)getgid(), &g)) {
        puts(g.name);
      } else {
        printf("%u\n", (unsigned)getgid());
      }
    }
  } else if (user) {
    printf("%u\n", (unsigned)getuid());
  } else if (group) {
    printf("%u\n", (unsigned)getgid());
  } else if (groups) {
    printf("%u\n", (unsigned)getgid());
  } else {
    struct user_entry u;
    struct group_entry g;
    const char *uname = user_by_uid((unsigned)getuid(), &u) ? u.name : "?";
    const char *gname = group_by_gid((unsigned)getgid(), &g) ? g.name : "?";
    printf("uid=%u(%s) gid=%u(%s) groups=%u(%s)\n", (unsigned)getuid(), uname, (unsigned)getgid(), gname,
           (unsigned)getgid(), gname);
  }
  return EXIT_SUCCESS;
}
