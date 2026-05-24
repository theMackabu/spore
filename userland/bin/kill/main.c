#include <spore.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

struct signal_name {
  const char *name;
  int value;
};

static const struct signal_name signals[] = {
  {"INT", SIGINT}, {"SIGINT", SIGINT}, {"KILL", SIGKILL}, {"SIGKILL", SIGKILL}, {"TERM", SIGTERM}, {"SIGTERM", SIGTERM},
};

static bool parse_signal(const char *text, int *out) {
  if (text == NULL || text[0] == '\0') { return false; }
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (end != text && *end == '\0' && value > 0 && value < 128) {
    *out = (int)value;
    return true;
  }
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
    if (streq(text, signals[i].name)) {
      *out = signals[i].value;
      return true;
    }
  }
  return false;
}

static void list_signals(void) {
  puts("INT KILL TERM");
}

int main(int argc, char **argv) {
  int sig = SIGTERM;
  int first = 1;
  if (argc == 2 && streq(argv[1], "-l")) {
    list_signals();
    return EXIT_SUCCESS;
  }
  if (argc > 2 && (streq(argv[1], "-s") || streq(argv[1], "--signal"))) {
    if (!parse_signal(argv[2], &sig)) {
      fprintf(stderr, "kill: unknown signal: %s\n", argv[2]);
      return EXIT_FAILURE;
    }
    first = 3;
  } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] != '\0') {
    if (!parse_signal(argv[1] + 1, &sig)) {
      fprintf(stderr, "kill: unknown signal: %s\n", argv[1] + 1);
      return EXIT_FAILURE;
    }
    first = 2;
  }
  if (first >= argc) { return usage("kill", "[-l] [-s SIGNAL|-SIGNAL] PID..."); }
  int rc = EXIT_SUCCESS;
  for (int i = first; i < argc; ++i) {
    char *end = NULL;
    long pid = strtol(argv[i], &end, 10);
    if (end == argv[i] || *end != '\0') {
      fprintf(stderr, "kill: invalid pid: %s\n", argv[i]);
      rc = EXIT_FAILURE;
      continue;
    }
    if (kill((pid_t)pid, sig) != 0) {
      perror(argv[i]);
      rc = EXIT_FAILURE;
    }
  }
  return rc;
}
