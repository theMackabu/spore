#include <spore.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct signal_case {
  const char *name;
  int value;
};

static const struct signal_case cases[] = {
  {"SIGINT", SIGINT},
  {"SIGTERM", SIGTERM},
  {"SIGSEGV", SIGSEGV},
  {"SIGKILL", SIGKILL},
};

static unsigned long monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)ts.tv_nsec / 1000000ul;
}

static int parse_signal(const char *text) {
  if (text == NULL) { return SIGTERM; }
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (streq(text, cases[i].name) || streq(text, cases[i].name + 3)) { return cases[i].value; }
  }
  return atoi(text);
}

static const char *signal_name(int sig) {
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (cases[i].value == sig) { return cases[i].name; }
  }
  return "SIGNAL";
}

static void spin_ms(unsigned long ms) {
  unsigned long end = monotonic_ms() + ms;
  while (monotonic_ms() < end) {}
}

static int run_one(int sig) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return EXIT_FAILURE;
  }
  if (pid == 0) {
    for (;;) {
      spin_ms(100);
    }
  }
  spin_ms(50);
  if (kill(pid, sig) != 0) {
    perror("kill");
    return EXIT_FAILURE;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return EXIT_FAILURE;
  }
  if (!WIFSIGNALED(status) || WTERMSIG(status) != sig) {
    printf("signal-crash: signal %d -> unexpected status 0x%x\n", sig, status);
    return EXIT_FAILURE;
  }
  printf("signal-crash: signal %d -> killed child %d\n", sig, (int)pid);
  return EXIT_SUCCESS;
}

static void crash_with(int sig) {
  if (sig == SIGSEGV) {
    puts("signal-crash: taking SIGSEGV by writing through a bad pointer");
    fflush(stdout);
    volatile int *bad = (volatile int *)(uintptr_t)0xdead;
    *bad = 1;
    for (;;) {}
  }
  printf("signal-crash: sending %s to self pid %d\n", signal_name(sig), (int)getpid());
  fflush(stdout);
  (void)kill(getpid(), sig);
  for (;;) {}
}

static int interactive(void) {
  char line[64];
  puts("signal-crash: type SIGINT, SIGTERM, SIGKILL, or SIGSEGV");
  fputs("signal> ", stdout);
  fflush(stdout);
  if (fgets(line, sizeof(line), stdin) == NULL) { return EXIT_FAILURE; }
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
  int sig = parse_signal(line);
  if (sig <= 0) {
    fprintf(stderr, "signal-crash: unknown signal: %s\n", line);
    return EXIT_FAILURE;
  }
  if (!isatty(STDIN_FILENO)) { return run_one(sig); }
  crash_with(sig);
  return EXIT_FAILURE;
}

int main(int argc, char **argv) {
  if (argc <= 1) { return interactive(); }
  if (streq(argv[1], "all")) {
    int rc = EXIT_SUCCESS;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      if (cases[i].value == SIGSEGV) { continue; }
      if (run_one(cases[i].value) != EXIT_SUCCESS) { rc = EXIT_FAILURE; }
    }
    return rc;
  }
  if (streq(argv[1], "sleep")) {
    printf("signal-crash: pid %d waiting for signal\n", (int)getpid());
    fflush(stdout);
    for (;;) {
      spin_ms(100);
    }
  }
  crash_with(parse_signal(argv[1]));
  return EXIT_FAILURE;
}
