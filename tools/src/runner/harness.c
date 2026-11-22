#define _GNU_SOURCE

#include "run_modes.h"
#include "runner.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum {
  BUF_CAP = 262144,
  STARTUP_SERIAL_CAP = 32768,
};

static volatile sig_atomic_t resize_pending;

static bool contains(const char *haystack, const char *needle) {
  return strstr(haystack, needle) != NULL;
}

void restore_host_terminal_mouse(void) {
  static const char reset[] = "\033[?1000l\033[?1002l\033[?1003l\033[?1006l\033[?1015l\033[?2004l";
  if (isatty(STDOUT_FILENO)) { (void)write(STDOUT_FILENO, reset, sizeof(reset) - 1); }
}

void write_serial_input(int fd, const char *buf, size_t len) {
  enum { CHUNK = 32 };
  for (size_t off = 0; off < len;) {
    size_t todo = len - off;
    if (todo > CHUNK) { todo = CHUNK; }
    ssize_t n = write(fd, buf + off, todo);
    if (n < 0) {
      if (errno == EINTR) { continue; }
      return;
    }
    if (n == 0) { return; }
    off += (size_t)n;
    if (off < len) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
      (void)nanosleep(&ts, NULL);
    }
  }
}

static void on_sigwinch(int sig) {
  (void)sig;
  resize_pending = 1;
}

static bool enter_raw_terminal(struct termios *saved) {
  if (!isatty(STDIN_FILENO)) { return false; }
  if (tcgetattr(STDIN_FILENO, saved) != 0) { return false; }
  struct termios raw = *saved;
  cfmakeraw(&raw);
  return tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
}

static void restore_terminal(bool raw_enabled, const struct termios *saved) {
  if (raw_enabled) { (void)tcsetattr(STDIN_FILENO, TCSANOW, saved); }
}

static int finish_harness(int status, bool raw_terminal, const struct termios *saved_termios) {
  restore_terminal(raw_terminal, saved_termios);
  return status;
}

double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

struct startup_serial {
  char buf[STARTUP_SERIAL_CAP];
  size_t len;
  bool done;
};

static void write_raw_to(const char *data, size_t len, FILE *out) {
  fwrite(data, 1, len, out);
  fflush(out);
}

static void terminal_size(unsigned *rows, unsigned *cols) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row >= 8 && ws.ws_col >= 40) {
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return;
  }
  *rows = 38;
  *cols = 96;
}

static void send_terminal_size(int input_fd) {
  unsigned rows = 0;
  unsigned cols = 0;
  terminal_size(&rows, &cols);
  char msg[40];
  int len = snprintf(msg, sizeof(msg), "\033]777;%u;%u\a", rows, cols);
  if (len > 0) { (void)write(input_fd, msg, (size_t)len); }
}

static void startup_append(struct startup_serial *startup, const char *chunk, size_t n) {
  if (n > sizeof(startup->buf) - startup->len) {
    size_t keep = sizeof(startup->buf) / 2;
    if (startup->len > keep) {
      memmove(startup->buf, startup->buf + startup->len - keep, keep);
      startup->len = keep;
    }
    if (n > sizeof(startup->buf) - startup->len) {
      chunk += n - (sizeof(startup->buf) - startup->len);
      n = sizeof(startup->buf) - startup->len;
    }
  }
  memcpy(startup->buf + startup->len, chunk, n);
  startup->len += n;
}

ssize_t find_shell_prompt(const char *data, size_t len) {
  for (size_t i = 0; i + 2 <= len; ++i) {
    if ((data[i] == '$' || data[i] == '#') && data[i + 1] == ' ') {
      size_t start = i;
      while (start > 0 && data[start - 1] != '\n' && data[start - 1] != '\r') {
        --start;
      }
      bool old_prompt = i > 0 && data[i - 1] == ' ';
      bool user_host_prompt = false;
      for (size_t j = start; j < i; ++j) {
        if (data[j] == '@') {
          for (size_t k = j + 1; k < i; ++k) {
            if (data[k] == ':') {
              user_host_prompt = true;
              break;
            }
          }
          break;
        }
      }
      bool spore_prompt = false;
      size_t context_start = i > 160 ? i - 160 : start;
      for (size_t j = context_start; j + strlen("spore.local") <= i; ++j) {
        if (memcmp(data + j, "spore.local", strlen("spore.local")) == 0) {
          spore_prompt = true;
          break;
        }
      }
      if (old_prompt || user_host_prompt || spore_prompt) { return (ssize_t)start; }
    }
  }
  return -1;
}

static void write_serial_output(const char *chunk, size_t n, struct startup_serial *startup) {
  if (startup->done) {
    write_raw_to(chunk, n, stdout);
    return;
  }

  startup_append(startup, chunk, n);
  static const char handoff[] = "[kernel] Limine handoff";
  ssize_t handoff_at = -1;
  for (size_t i = 0; i + sizeof(handoff) - 1 <= startup->len; ++i) {
    if (memcmp(startup->buf + i, handoff, sizeof(handoff) - 1) == 0) {
      handoff_at = (ssize_t)i;
      break;
    }
  }
  if (handoff_at < 0) { handoff_at = find_shell_prompt(startup->buf, startup->len); }
  if (handoff_at < 0) { return; }

  write_raw_to(startup->buf + handoff_at, startup->len - (size_t)handoff_at, stdout);
  startup->done = true;
  startup->len = 0;
}

static void flush_startup_serial(struct startup_serial *startup) {
  if (startup == NULL || startup->done || startup->len == 0) { return; }
  write_raw_to(startup->buf, startup->len, stdout);
  startup->done = true;
  startup->len = 0;
}

struct boot_milestone {
  const char *needle;
  const char *label;
  bool seen;
  double at;
};

static void report_milestones(struct boot_milestone *milestones, size_t count, const char *buf, double start) {
  for (size_t i = 0; i < count; ++i) {
    if (!milestones[i].seen && contains(buf, milestones[i].needle)) {
      milestones[i].seen = true;
      milestones[i].at = now_seconds() - start;
    }
  }
}

static void print_timing_summary(FILE *out, const struct boot_milestone *milestones, size_t count,
                                 double first_output_at) {
  fputs("\r\n[host] boot timings:\r\n", out);
  if (first_output_at >= 0.0) { fprintf(out, "[host]   +%.3fs first qemu output\r\n", first_output_at); }
  for (size_t i = 0; i < count; ++i)
    if (milestones[i].seen) { fprintf(out, "[host]   +%.3fs %s\r\n", milestones[i].at, milestones[i].label); }
  fflush(out);
}

static void append(char *buf, size_t *len, const char *data, size_t n) {
  if (*len + n >= BUF_CAP) {
    size_t drop = (*len + n) - (BUF_CAP - 1);
    if (drop > *len) { drop = *len; }
    memmove(buf, buf + drop, *len - drop);
    *len -= drop;
  }
  memcpy(buf + *len, data, n);
  *len += n;
  buf[*len] = '\0';
}

static void record_output(char *buf, size_t *len, const char *chunk, size_t n, struct boot_milestone *milestones,
                          size_t milestone_count, bool timings, double start, bool *first_output,
                          double *first_output_at, bool *timing_summary_printed, FILE *log_stream) {
  if (timings && !*first_output) {
    *first_output = true;
    *first_output_at = now_seconds() - start;
  }
  append(buf, len, chunk, n);
  if (timings) {
    report_milestones(milestones, milestone_count, buf, start);
    if (!*timing_summary_printed && milestones[milestone_count - 1].seen) {
      *timing_summary_printed = true;
      print_timing_summary(log_stream, milestones, milestone_count, *first_output_at);
    }
  }
}

static void drain_available(int fd, char *buf, size_t *len, bool print_boot_output, bool mirror, FILE *mirror_stream,
                            struct startup_serial *startup) {
  int old_flags = fcntl(fd, F_GETFL, 0);
  if (old_flags >= 0) { (void)fcntl(fd, F_SETFL, old_flags | O_NONBLOCK); }

  char chunk[4096];
  for (;;) {
    ssize_t n = read(fd, chunk, sizeof(chunk));
    if (n > 0) {
      if (print_boot_output) { write_serial_output(chunk, (size_t)n, startup); }
      if (mirror) { write_raw_to(chunk, (size_t)n, mirror_stream); }
      append(buf, len, chunk, (size_t)n);
      continue;
    }
    if (n < 0 && errno == EINTR) { continue; }
    break;
  }

  if (old_flags >= 0) { (void)fcntl(fd, F_SETFL, old_flags); }
}

int run_harness(char **qemu_argv, const char *mode, bool timings) {
  int in_pipe[2];
  int serial_pipe[2];
  if (pipe(in_pipe) != 0 || pipe(serial_pipe) != 0) {
    perror("pipe");
    return 1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    dup2(in_pipe[0], STDIN_FILENO);
    dup2(serial_pipe[1], STDOUT_FILENO);
    dup2(serial_pipe[1], STDERR_FILENO);
    close(in_pipe[0]);
    close(in_pipe[1]);
    close(serial_pipe[0]);
    close(serial_pipe[1]);
    execvp(qemu_argv[0], qemu_argv);
    perror(qemu_argv[0]);
    _exit(127);
  }
  close(in_pipe[0]);
  close(serial_pipe[1]);
  pid_t echo_pid = start_udp_echo_server();
  pid_t http_pid = start_http_server();
  (void)signal(SIGWINCH, on_sigwinch);

  char buf[BUF_CAP] = {0};
  size_t len = 0;
  bool stdin_sent = false;
  bool shell_check_failed = false;
  bool shell_size_sent = false;
  struct shell_check shell_state = {0};
  bool plain = strcmp(mode, "plain") == 0;
  bool shell = strcmp(mode, "shell") == 0;
  bool bench = strcmp(mode, "bench") == 0;
  bool rng = strcmp(mode, "rng") == 0;
  bool interactive = plain;
  struct bench_run bench_state = {0};
  struct rng_run rng_state = {0};
  struct termios saved_termios;
  bool raw_terminal = interactive && enter_raw_terminal(&saved_termios);
  double start = now_seconds();
  double deadline = plain ? 0.0 : start + (shell ? 300.0 : 120.0);
  shell_state.last_output = start;
  bool first_output = false;
  double first_output_at = -1.0;
  bool timing_summary_printed = false;
  struct startup_serial startup_serial = {0};
  FILE *log_stream = stderr;
  struct boot_milestone milestones[] = {
    {"UEFI firmware", "edk2 first banner", false, 0.0},
    {"BdsDxe: starting", "edk2 starts boot option", false, 0.0},
    {"[kernel] Limine handoff", "Limine entered kernel", false, 0.0},
    {"[kernel] booted", "kernel first log", false, 0.0},
    {"[kernel] entering EL0", "enter EL0", false, 0.0},
    {"$ ", "shell prompt", false, 0.0},
  };
  if (timings) {
    fprintf(log_stream, "[host] qemu launched; timing summary prints at shell prompt\r\n");
    fflush(log_stream);
  }
  for (;;) {
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) {
      drain_available(serial_pipe[0], buf, &len, plain || shell, false, stderr, &startup_serial);
      if (plain || shell) { flush_startup_serial(&startup_serial); }
      if (rng && rng_state.sample[0] != '\0') { printf("[rng] %s\n", rng_state.sample); }
      if (strcmp(mode, "filter") == 0 || strcmp(mode, "stdin") == 0) {
        print_guest_checks(buf);
      } else if (!plain && !shell && !bench && !rng) {
        fputs(buf, stdout);
      }
      int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
      if (echo_pid > 0) {
        (void)kill(echo_pid, SIGTERM);
        (void)waitpid(echo_pid, NULL, 0);
      }
      if (http_pid > 0) {
        (void)kill(http_pid, SIGTERM);
        (void)waitpid(http_pid, NULL, 0);
      }
      if (shell_check_failed) { rc = 1; }
      return finish_harness(rc, raw_terminal, &saved_termios);
    }
    if (!plain && now_seconds() > deadline) {
      kill(pid, SIGTERM);
      waitpid(pid, NULL, 0);
      if (echo_pid > 0) {
        (void)kill(echo_pid, SIGTERM);
        (void)waitpid(echo_pid, NULL, 0);
      }
      if (http_pid > 0) {
        (void)kill(http_pid, SIGTERM);
        (void)waitpid(http_pid, NULL, 0);
      }
      return finish_harness(124, raw_terminal, &saved_termios);
    }
    if (shell_size_sent && resize_pending) {
      resize_pending = 0;
      send_terminal_size(in_pipe[1]);
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(serial_pipe[0], &rfds);
    if (interactive) { FD_SET(STDIN_FILENO, &rfds); }
    int max_fd = serial_pipe[0];
    if (interactive && STDIN_FILENO > max_fd) { max_fd = STDIN_FILENO; }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
    int ready = select(max_fd + 1, &rfds, NULL, NULL, &tv);
    if (ready > 0) {
      if (interactive && FD_ISSET(STDIN_FILENO, &rfds)) {
        char input[1024];
        ssize_t n = read(STDIN_FILENO, input, sizeof(input));
        if (n > 0) { write_serial_input(in_pipe[1], input, (size_t)n); }
      }
      char chunk[1024];
      if (FD_ISSET(serial_pipe[0], &rfds)) {
        ssize_t n = read(serial_pipe[0], chunk, sizeof(chunk));
        if (n > 0) {
          if (shell) { shell_state.last_output = now_seconds(); }
          record_output(buf, &len, chunk, (size_t)n, milestones, sizeof(milestones) / sizeof(milestones[0]), timings,
                        start, &first_output, &first_output_at, &timing_summary_printed, log_stream);
          if (plain || shell) { write_serial_output(chunk, (size_t)n, &startup_serial); }
        }
      }
      if (len > 0) {
        if (!shell_size_sent && find_shell_prompt(buf, len) >= 0) {
          resize_pending = 0;
          send_terminal_size(in_pipe[1]);
          shell_size_sent = true;
        }
        if (strcmp(mode, "stdin") == 0 && !stdin_sent) { stdin_sent = answer_guest_stdin(in_pipe[1], buf); }
        if (shell && !shell_check_output(&shell_state, in_pipe[1], buf, &len)) {
          shell_check_failed = true;
          kill(pid, SIGTERM);
        }
        if ((bench || rng) && find_shell_prompt(buf, len) >= 0) {
          if (bench) { bench_prompt(&bench_state, in_pipe[1]); }
          if (rng) { rng_prompt(&rng_state, in_pipe[1], buf); }
          buf[0] = '\0';
          len = 0;
        }
      }
    } else if (ready == 0 && shell) {
      shell_check_idle(&shell_state, in_pipe[1], buf, &len);
    }
  }
}
