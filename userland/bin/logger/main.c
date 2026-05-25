#include <errno.h>
#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void timestamp(char *buf, size_t cap) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

static int append_log(const char *path, const char *line) {
  FILE *f = fopen(path, "a");
  if (f == NULL) { return -1; }
  fputs(line, f);
  fclose(f);
  return 0;
}

int main(int argc, char **argv) {
  const char *tag = "logger";
  int arg = 1;
  if (argc > 2 && streq(argv[1], "-t")) {
    tag = argv[2];
    arg = 3;
  }
  if (arg >= argc) { return usage("logger", "[-t TAG] MESSAGE..."); }

  char msg[512] = {0};
  for (int i = arg; i < argc; ++i) {
    if (i > arg) { strncat(msg, " ", sizeof(msg) - strlen(msg) - 1); }
    strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
  }

  char ts[32];
  timestamp(ts, sizeof(ts));
  char line[640];
  snprintf(line, sizeof(line), "%s %s[%d]: %s\n", ts, tag, (int)getpid(), msg);
  if (append_log("/var/log/messages", line) != 0 || append_log("/var/log/syslog", line) != 0) {
    eprintf("logger: /var/log: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
