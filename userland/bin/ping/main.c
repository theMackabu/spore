#include <spore.h>

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif

enum { DEFAULT_COUNT = 4, DEFAULT_SIZE = 56, MAX_PAYLOAD = 1400 };

static volatile sig_atomic_t interrupted;

static void on_sigint(int signal) {
  (void)signal;
  interrupted = 1;
}

static uint16_t be16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

static void ping_usage(void) {
  puts("usage: ping [-nq] [-c count] [-i interval] [-s size] [-W timeout] host");
  puts("  -c count     stop after count replies/attempts");
  puts("  -i seconds   wait interval seconds between packets");
  puts("  -s size      payload bytes");
  puts("  -W seconds   per-packet timeout");
  puts("  -n           numeric output only");
  puts("  -q           quiet summary only");
}

static long parse_long(const char *s, long min, long max, const char *opt) {
  char *end = NULL;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
    fprintf(stderr, "ping: invalid %s: %s\n", opt, s);
    exit(2);
  }
  return v;
}

static double parse_double(const char *s, double min, double max, const char *opt) {
  char *end = NULL;
  errno = 0;
  double v = strtod(s, &end);
  if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
    fprintf(stderr, "ping: invalid %s: %s\n", opt, s);
    exit(2);
  }
  return v;
}

static double monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0.0; }
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void sleep_seconds(double seconds) {
  if (seconds <= 0.0) { return; }
  struct timespec ts;
  ts.tv_sec = (time_t)seconds;
  ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1000000000.0);
  while (!interrupted && nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

int main(int argc, char **argv) {
  long count = DEFAULT_COUNT;
  long size = DEFAULT_SIZE;
  double interval = 1.0;
  double timeout = 1.0;
  bool quiet = false;
  bool numeric = false;
  const char *target = NULL;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      ping_usage();
      return 0;
    } else if (strcmp(argv[i], "-q") == 0) {
      quiet = true;
    } else if (strcmp(argv[i], "-n") == 0) {
      numeric = true;
    } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      count = parse_long(argv[++i], 1, 1000000, "count");
    } else if (strncmp(argv[i], "-c", 2) == 0 && argv[i][2] != '\0') {
      count = parse_long(argv[i] + 2, 1, 1000000, "count");
    } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      interval = parse_double(argv[++i], 0.0, 3600.0, "interval");
    } else if (strncmp(argv[i], "-i", 2) == 0 && argv[i][2] != '\0') {
      interval = parse_double(argv[i] + 2, 0.0, 3600.0, "interval");
    } else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
      timeout = parse_double(argv[++i], 0.001, 3600.0, "timeout");
    } else if (strncmp(argv[i], "-W", 2) == 0 && argv[i][2] != '\0') {
      timeout = parse_double(argv[i] + 2, 0.001, 3600.0, "timeout");
    } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      size = parse_long(argv[++i], 0, MAX_PAYLOAD, "size");
    } else if (strncmp(argv[i], "-s", 2) == 0 && argv[i][2] != '\0') {
      size = parse_long(argv[i] + 2, 0, MAX_PAYLOAD, "size");
    } else if (argv[i][0] == '-') {
      fprintf(stderr, "ping: unknown option: %s\n", argv[i]);
      ping_usage();
      return 2;
    } else if (target == NULL) {
      target = argv[i];
    } else {
      fprintf(stderr, "ping: extra operand: %s\n", argv[i]);
      return 2;
    }
  }

  if (target == NULL) {
    ping_usage();
    return 2;
  }

  uint32_t ip;
  if (!resolve_ipv4(target, &ip)) {
    fprintf(stderr, "ping: bad address: %s\n", target);
    return 1;
  }

  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  if (fd < 0) {
    perror("socket");
    return 1;
  }

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = be16(0);
  sa.sin_addr.s_addr = ip;

  char ip_s[32];
  format_ipv4(ip, ip_s, sizeof(ip_s));
  const char *shown = numeric ? ip_s : target;
  if (!quiet) { printf("PING %s (%s): %ld data bytes\n", shown, ip_s, size); }
  (void)signal(SIGINT, on_sigint);

  char payload[MAX_PAYLOAD];
  char buf[1600];
  long transmitted = 0;
  long received = 0;
  double min_ms = 0.0;
  double max_ms = 0.0;
  double sum_ms = 0.0;

  for (long seq = 1; seq <= count && !interrupted; ++seq) {
    for (long i = 0; i < size; ++i) {
      payload[i] = (char)('a' + (i % 26));
    }
    double start = monotonic_ms();
    ++transmitted;
    if (sendto(fd, payload, (size_t)size, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
      perror("sendto");
      close(fd);
      return 1;
    }

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int timeout_ms = (int)(timeout * 1000.0);
    if (timeout_ms < 1) { timeout_ms = 1; }
    int prc = poll(&pfd, 1, timeout_ms);
    if (prc < 0 && errno == EINTR && interrupted) { break; }
    if (prc > 0 && (pfd.revents & POLLIN) != 0) {
      ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
      double elapsed = monotonic_ms() - start;
      if (n >= 0) {
        ++received;
        if (received == 1 || elapsed < min_ms) { min_ms = elapsed; }
        if (received == 1 || elapsed > max_ms) { max_ms = elapsed; }
        sum_ms += elapsed;
        if (!quiet) { printf("%zd bytes from %s: icmp_seq=%ld time=%.3f ms\n", n, ip_s, seq, elapsed); }
      } else if (!quiet) {
        perror("recvfrom");
      }
    } else if (!quiet) {
      printf("Request timeout for icmp_seq %ld\n", seq);
    }
    if (seq != count) { sleep_seconds(interval); }
  }

  long loss = transmitted == 0 ? 0 : ((transmitted - received) * 100) / transmitted;
  printf("--- %s ping statistics ---\n", shown);
  printf("%ld packets transmitted, %ld packets received, %ld%% packet loss\n", transmitted, received, loss);
  if (received > 0) { printf("round-trip min/avg/max = %.3f/%.3f/%.3f ms\n", min_ms, sum_ms / received, max_ms); }
  close(fd);
  if (interrupted) { return 130; }
  return received == 0 ? 1 : 0;
}
