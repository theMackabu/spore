#define _DEFAULT_SOURCE
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved;
static bool raw;
static volatile sig_atomic_t resized;
static volatile sig_atomic_t interrupted;

static volatile sig_atomic_t event_fd = -1;

int runner_inherit_fd(int fd) {
  return fcntl(fd, F_SETFD, 0);
}

static const int signals[] = {SIGWINCH, SIGTERM, SIGHUP, SIGINT, SIGCHLD};
static struct sigaction previous[sizeof(signals) / sizeof(signals[0])];
static size_t installed;

static void notify(int sig) {
  if (sig == SIGWINCH) {
    resized = 1;
  } else if (sig != SIGCHLD) {
    interrupted = sig;
  }
  if (event_fd >= 0) {
    int saved_errno = errno;
    char byte = 1;
    (void)write(event_fd, &byte, 1);
    errno = saved_errno;
  }
}

void runner_terminal_open(bool interactive, int events) {
  event_fd = events;
  interrupted = resized = 0;
  struct sigaction sa = {.sa_handler = notify};
  sigemptyset(&sa.sa_mask);
  installed = sizeof(signals) / sizeof(signals[0]);
  if (events < 0) { --installed; }
  for (size_t i = 0; i < installed; ++i) { sigaction(signals[i], &sa, &previous[i]); }
  if (interactive && isatty(0) && tcgetattr(0, &saved) == 0) {
    struct termios settings = saved;
    cfmakeraw(&settings);
    raw = tcsetattr(0, TCSANOW, &settings) == 0;
  }
}

void runner_terminal_close(void) {
  event_fd = -1;
  if (raw) { tcsetattr(0, TCSANOW, &saved); raw = false; }
  while (installed > 0) { --installed; sigaction(signals[installed], &previous[installed], NULL); }
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
  if (ioctl(1, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col) {
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

void runner_kill_group(int pid) {
  if (pid > 0) { kill(-pid, SIGKILL); }
}
