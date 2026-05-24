#include <spore.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static bool parse_group(const char *s, unsigned *out) {
  if (s[0] >= '0' && s[0] <= '9') {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end != NULL && *end == '\0') {
      *out = (unsigned)v;
      return true;
    }
  }
  struct group_entry group;
  if (group_by_name(s, &group)) {
    *out = group.gid;
    return true;
  }
  return false;
}

int main(int argc, char **argv) {
  if (argc < 3) { return usage("chgrp", "GROUP FILE..."); }
  unsigned gid = 0;
  if (!parse_group(argv[1], &gid)) {
    eprintf("chgrp: unknown group: %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  int rc = EXIT_SUCCESS;
  for (int i = 2; i < argc; ++i) {
    if (fchownat(AT_FDCWD, argv[i], (uid_t)-1, gid, 0) != 0) {
      perror(argv[i]);
      rc = EXIT_FAILURE;
    }
  }
  return rc;
}
