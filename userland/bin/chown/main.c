#include <spore.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool parse_id(const char *s, bool group, unsigned *out) {
  if (s[0] >= '0' && s[0] <= '9') {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end != NULL && *end == '\0') {
      *out = (unsigned)v;
      return true;
    }
  }
  if (group) {
    struct group_entry g;
    if (group_by_name(s, &g)) {
      *out = g.gid;
      return true;
    }
  } else {
    struct user_entry u;
    if (user_by_name(s, &u)) {
      *out = u.uid;
      return true;
    }
  }
  return false;
}

int main(int argc, char **argv) {
  if (argc < 3) { return usage("chown", "OWNER[:GROUP] FILE..."); }
  char spec[96];
  snprintf(spec, sizeof(spec), "%s", argv[1]);
  char *colon = strchr(spec, ':');
  if (colon != NULL) { *colon++ = '\0'; }
  unsigned uid = (unsigned)-1;
  unsigned gid = (unsigned)-1;
  if (spec[0] != '\0' && !parse_id(spec, false, &uid)) {
    eprintf("chown: unknown user: %s\n", spec);
    return EXIT_FAILURE;
  }
  if (colon != NULL && colon[0] != '\0' && !parse_id(colon, true, &gid)) {
    eprintf("chown: unknown group: %s\n", colon);
    return EXIT_FAILURE;
  }
  int rc = EXIT_SUCCESS;
  for (int i = 2; i < argc; ++i) {
    if (fchownat(AT_FDCWD, argv[i], uid, gid, 0) != 0) {
      perror(argv[i]);
      rc = EXIT_FAILURE;
    }
  }
  return rc;
}
