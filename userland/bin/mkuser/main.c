#include <spore.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool valid_name(const char *s) {
  if (!(s[0] == '_' || isalpha((unsigned char)s[0]))) { return false; }
  for (const char *p = s + 1; *p != '\0'; ++p) {
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') { return false; }
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) { return usage("mkuser", "NAME"); }
  if (getuid() != 0) {
    eprintf("mkuser: root required\n");
    return EXIT_FAILURE;
  }
  if (!valid_name(argv[1])) {
    eprintf("mkuser: invalid user name\n");
    return EXIT_FAILURE;
  }
  struct user_entry existing;
  if (user_by_name(argv[1], &existing)) {
    eprintf("mkuser: user exists: %s\n", argv[1]);
    return EXIT_FAILURE;
  }
  unsigned uid = next_user_id();
  char home[160];
  snprintf(home, sizeof(home), "/home/%s", argv[1]);
  FILE *pw = fopen("/etc/passwd", "a");
  FILE *gr = fopen("/etc/group", "a");
  if (pw == NULL || gr == NULL) {
    perror("mkuser");
    if (pw != NULL) { fclose(pw); }
    if (gr != NULL) { fclose(gr); }
    return EXIT_FAILURE;
  }
  fprintf(pw, "%s:x:%u:%u:%s:%s:/bin/msh\n", argv[1], uid, uid, argv[1], home);
  fprintf(gr, "%s:x:%u:\n", argv[1], uid);
  fclose(pw);
  fclose(gr);
  if (!add_shadow_user(argv[1])) { perror("/etc/shadow"); }
  if (mkdir(home, 0755) != 0) { perror(home); }
  (void)chown(home, uid, uid);
  printf("created user %s uid=%u home=%s\n", argv[1], uid, home);
  return EXIT_SUCCESS;
}
