#include <errno.h>
#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_pselect6
constexpr int SYS_pselect6 = 72;
#endif

static int parse_delay(const char *arg, unsigned long *seconds) {
  if (streq(arg, "now")) {
    *seconds = 0;
    return 0;
  }
  if (arg[0] == '+') { ++arg; }
  if (arg[0] == '\0') { return -1; }
  char *end = NULL;
  unsigned long value = strtoul(arg, &end, 10);
  if (end == arg || *end != '\0') { return -1; }
  *seconds = value;
  return 0;
}

static int sleep_seconds(unsigned long seconds) {
  while (seconds > 0) {
    struct timespec ts = {
      .tv_sec = seconds > 3600 ? 3600 : (time_t)seconds,
      .tv_nsec = 0,
    };
    long rc = syscall(SYS_pselect6, 0, 0, 0, 0, &ts, 0);
    if (rc < 0 && errno != EINTR) {
      perror("shutdown: sleep");
      return -1;
    }
    seconds -= (unsigned long)ts.tv_sec;
    if (ts.tv_sec == 0) { break; }
  }
  return 0;
}

static int mycelium_poweroff(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("shutdown: socket");
    return -1;
  }
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof(sa.sun_path), "/run/mycelium.sock");
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    perror("shutdown: connect /run/mycelium.sock");
    close(fd);
    return -1;
  }
  (void)write(fd, "poweroff\n", 9);
  char buf[256];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) { break; }
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }
  close(fd);
  return 0;
}

int main(int argc, char **argv) {
  unsigned long delay = 0;
  if (argc > 2 || (argc == 2 && streq(argv[1], "--help"))) { return usage("shutdown", "[now|SECONDS]"); }
  if (argc == 2 && parse_delay(argv[1], &delay) != 0) { return usage("shutdown", "[now|SECONDS]"); }

  if (geteuid() != 0) {
    eprintf("shutdown: permission denied; try sudo shutdown\n");
    return EXIT_FAILURE;
  }

  if (delay > 0) {
    printf("shutdown: scheduled in %lu seconds\n", delay);
    fflush(stdout);
    if (sleep_seconds(delay) != 0) { return EXIT_FAILURE; }
  }

  if (mycelium_poweroff() != 0) { return EXIT_FAILURE; }
  for (;;) {
    pause();
  }
}
