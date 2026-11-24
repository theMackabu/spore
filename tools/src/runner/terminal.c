#define _DEFAULT_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved;
static bool raw;
static volatile sig_atomic_t resized;
static volatile sig_atomic_t interrupted;

static void notify(int sig) {
  if (sig == SIGWINCH) {
    resized = 1;
  } else {
    interrupted = sig;
  }
}

void runner_terminal_open(bool interactive) {
  struct sigaction sa = {.sa_handler = notify};
  sigemptyset(&sa.sa_mask);
  sigaction(SIGWINCH, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  if (interactive && isatty(0) && tcgetattr(0, &saved) == 0) {
    struct termios settings = saved;
    cfmakeraw(&settings);
    raw = tcsetattr(0, TCSANOW, &settings) == 0;
  }
}

void runner_terminal_close(void) {
  if (raw) { tcsetattr(0, TCSANOW, &saved); }
}

int runner_interrupted(void) {
  return interrupted;
}

bool runner_resized(void) {
  if (!resized) { return false; }
  resized = 0;
  return true;
}

void runner_terminal_size(unsigned short *rows, unsigned short *cols) {
  struct winsize size;
  if (ioctl(1, TIOCGWINSZ, &size) == 0 && size.ws_row >= 8 && size.ws_col >= 40) {
    *rows = size.ws_row;
    *cols = size.ws_col;
  }
}

int runner_poll(int serial, int keyboard, int timeout) {
  struct pollfd fds[] = {{.fd = serial, .events = POLLIN}, {.fd = keyboard, .events = POLLIN}};
  int rc = poll(fds, 2, timeout);
  if (rc < 0) { return errno == EINTR ? 0 : -1; }
  return (fds[0].revents ? 1 : 0) | (fds[1].revents ? 2 : 0);
}
