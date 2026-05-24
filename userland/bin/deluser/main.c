#include <spore.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool rewrite_without(const char *path, const char *name) {
  FILE *in = fopen(path, "r");
  FILE *out = fopen("/tmp/deluser.tmp", "w");
  if (in == NULL || out == NULL) {
    if (in != NULL) { fclose(in); }
    if (out != NULL) { fclose(out); }
    return false;
  }
  char line[384];
  size_t name_len = strlen(name);
  while (fgets(line, sizeof(line), in) != NULL) {
    if (strncmp(line, name, name_len) == 0 && line[name_len] == ':') { continue; }
    fputs(line, out);
  }
  fclose(in);
  fclose(out);
  return rename("/tmp/deluser.tmp", path) == 0;
}

int main(int argc, char **argv) {
  if (argc != 2) { return usage("deluser", "NAME"); }
  if (getuid() != 0) {
    eprintf("deluser: root required\n");
    return EXIT_FAILURE;
  }
  if (streq(argv[1], "root")) {
    eprintf("deluser: refusing to remove root\n");
    return EXIT_FAILURE;
  }
  if (!rewrite_without("/etc/passwd", argv[1]) || !rewrite_without("/etc/group", argv[1]) ||
      !remove_shadow_user(argv[1])) {
    perror("deluser");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
