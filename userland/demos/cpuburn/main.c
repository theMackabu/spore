#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
  MAX_WORKERS = 64,
  DEFAULT_SECONDS = 10,
};

static volatile bool stop_requested;
static volatile uint64_t counters[MAX_WORKERS];

struct worker_args {
  int index;
};

static void usage(void) {
  fputs("usage: cpuburn [-j workers] [-s seconds]\n", stderr);
  exit(2);
}

static int parse_positive(const char *text) {
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (text == NULL || *text == '\0' || end == NULL || *end != '\0' || value <= 0 || value > MAX_WORKERS) {
    usage();
  }
  return (int)value;
}

static int online_cpu_count(void) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    int count = CPU_COUNT(&set);
    if (count > 0) { return count > MAX_WORKERS ? MAX_WORKERS : count; }
  }

  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) { n = 1; }
  if (n > MAX_WORKERS) { n = MAX_WORKERS; }
  return (int)n;
}

static void *worker_main(void *arg) {
  const struct worker_args *worker = arg;
  uint64_t x = (uint64_t)(worker->index + 1) * 0x9e3779b97f4a7c15ull;

  while (!stop_requested) {
    x ^= x << 7;
    x ^= x >> 9;
    x += 0x165667b19e3779f9ull;
    ++counters[worker->index];
  }

  return (void *)(uintptr_t)x;
}

int main(int argc, char **argv) {
  int workers = 0;
  int seconds = DEFAULT_SECONDS;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-j") == 0) {
      if (++i >= argc) { usage(); }
      workers = parse_positive(argv[i]);
    } else if (strcmp(argv[i], "-s") == 0) {
      if (++i >= argc) { usage(); }
      seconds = parse_positive(argv[i]);
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage();
    } else {
      usage();
    }
  }

  if (workers == 0) { workers = online_cpu_count(); }

  pthread_t threads[MAX_WORKERS];
  struct worker_args args[MAX_WORKERS];
  uint64_t previous[MAX_WORKERS] = {0};

  printf("cpuburn: workers=%d seconds=%d online-cpus=%d\n", workers, seconds, online_cpu_count());
  fflush(stdout);

  for (int i = 0; i < workers; ++i) {
    args[i].index = i;
    int rc = pthread_create(&threads[i], NULL, worker_main, &args[i]);
    if (rc != 0) {
      fprintf(stderr, "cpuburn: pthread_create(%d): %s\n", i, strerror(rc));
      stop_requested = 1;
      workers = i;
      break;
    }
  }

  for (int sec = 1; !stop_requested && sec <= seconds; ++sec) {
    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
    while (!stop_requested && nanosleep(&ts, &ts) != 0 && errno == EINTR) {}

    printf("%2d:", sec);
    for (int i = 0; i < workers; ++i) {
      uint64_t current = counters[i];
      printf(" w%d=%llu", i, (unsigned long long)(current - previous[i]));
      previous[i] = current;
    }
    putchar('\n');
    fflush(stdout);
  }

  stop_requested = 1;
  for (int i = 0; i < workers; ++i) {
    (void)pthread_join(threads[i], NULL);
  }

  puts("cpuburn: done");
  return 0;
}
