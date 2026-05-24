#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum {
  BUF_CAP = 262144,
  PATH_CAP = 1024,
  STARTUP_SERIAL_CAP = 32768,
};

static const char *shell_commands[] = {
  "ls /bin\n",
  "ls /dev\n",
  "echo null-ok > /dev/null\n",
  "echo console-ok > /dev/console\n",
  "echo full > /dev/full\n",
  "cat /etc/motd\n",
  "echo hi > /tmp/f\n",
  "cat /tmp/f\n",
  "edit /tmp/edit-test\na\nfrom edit\n.\nw\nq\n",
  "cat /tmp/edit-test\n",
  "mkdir /tmp/d && cd /tmp/d && touch x && ls\n",
  "/bin/hello\n",
  "pthread-demo\n",
  "ping 10.0.2.2\n",
  "udp-echo 10.0.2.2 5555 hi\n",
  "confine net:none udp-send 10.0.2.2 5555 hi\n",
  "confine net:udp:10.0.2.2:5555 udp-send 10.0.2.2 5555 hi\n",
  "confine compute-only /demos/spinner\n",
  "confine fs:/tmp /demos/peeker /etc/motd\n",
  "confine fs:/tmp /demos/writer /tmp/d/out\n",
  "confine mem:1 /demos/memhog\n",
  "runc bad-manifest /demos/escalate\n",
  "shutdown\n",
};

static void usage(void) {
  fputs("usage: spore-run [--mode plain|filter|shell|stdin] [--timings] [--tmux-log-pane] --image IMAGE "
        "[--qemu QEMU] [--accel ACCEL] [--cpu CPU] [--vars VARS_FD]\n",
        stderr);
  exit(2);
}

static bool contains(const char *haystack, const char *needle) {
  return strstr(haystack, needle) != NULL;
}

static bool ends_with(const char *s, const char *suffix) {
  size_t s_len = strlen(s);
  size_t suffix_len = strlen(suffix);
  return s_len >= suffix_len && strcmp(s + s_len - suffix_len, suffix) == 0;
}

static bool find_firmware(const char *qemu, char *out, size_t cap) {
  char cmd[PATH_CAP];
  snprintf(cmd, sizeof(cmd), "%s -L help 2>/dev/null", qemu);
  FILE *pipe = popen(cmd, "r");
  if (pipe != NULL) {
    char dir[PATH_CAP];
    while (fscanf(pipe, "%1023s", dir) == 1) {
      snprintf(out, cap, "%s/edk2-aarch64-code.fd", dir);
      if (access(out, R_OK) == 0) {
        pclose(pipe);
        return true;
      }
      char *suffix = strstr(dir, "-firmware");
      if (suffix != NULL) { *suffix = '\0'; }
      snprintf(out, cap, "%s/qemu/edk2-aarch64-code.fd", dir);
      if (access(out, R_OK) == 0) {
        pclose(pipe);
        return true;
      }
    }
    pclose(pipe);
  }
  snprintf(out, cap, "/opt/homebrew/opt/qemu/share/qemu/edk2-aarch64-code.fd");
  return access(out, R_OK) == 0;
}

static bool find_vars_template(const char *qemu, char *out, size_t cap) {
  char cmd[PATH_CAP];
  snprintf(cmd, sizeof(cmd), "%s -L help 2>/dev/null", qemu);
  FILE *pipe = popen(cmd, "r");
  if (pipe != NULL) {
    char dir[PATH_CAP];
    while (fscanf(pipe, "%1023s", dir) == 1) {
      char base[PATH_CAP];
      snprintf(base, sizeof(base), "%s", dir);
      char *suffix = strstr(base, "-firmware");
      if (suffix != NULL) { *suffix = '\0'; }
      const char *patterns[] = {
        "%s/edk2-arm-vars.fd",
        "%s/edk2-aarch64-vars.fd",
        "%s/qemu/edk2-arm-vars.fd",
        "%s/qemu/edk2-aarch64-vars.fd",
      };
      for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
        snprintf(out, cap, patterns[i], i < 2 ? dir : base);
        if (access(out, R_OK) == 0) {
          pclose(pipe);
          return true;
        }
      }
    }
    pclose(pipe);
  }
  const char *fallbacks[] = {
    "/opt/homebrew/opt/qemu/share/qemu/edk2-arm-vars.fd",
    "/opt/homebrew/opt/qemu/share/qemu/edk2-aarch64-vars.fd",
  };
  for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i) {
    snprintf(out, cap, "%s", fallbacks[i]);
    if (access(out, R_OK) == 0) { return true; }
  }
  return false;
}

static void copy_file_or_die(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (in == NULL) {
    perror(src);
    exit(1);
  }
  FILE *out = fopen(dst, "wb");
  if (out == NULL) {
    perror(dst);
    fclose(in);
    exit(1);
  }
  char buf[65536];
  for (;;) {
    size_t n = fread(buf, 1, sizeof(buf), in);
    if (n > 0 && fwrite(buf, 1, n, out) != n) {
      perror(dst);
      exit(1);
    }
    if (n < sizeof(buf)) {
      if (ferror(in)) {
        perror(src);
        exit(1);
      }
      break;
    }
  }
  fclose(in);
  fclose(out);
}

static void ensure_vars_file(const char *qemu, const char *vars) {
  if (vars == NULL || access(vars, R_OK | W_OK) == 0) { return; }
  char tmpl[PATH_CAP];
  if (!find_vars_template(qemu, tmpl, sizeof(tmpl))) {
    fputs("spore-run: edk2 vars template not found\n", stderr);
    exit(1);
  }
  copy_file_or_die(tmpl, vars);
}

static FILE *open_tmux_log_pane(char *pane_id, size_t pane_id_cap) {
  pane_id[0] = '\0';
  if (getenv("TMUX") == NULL) {
    fputs("spore-run: --tmux-log-pane requested outside tmux; using stderr for logs\n", stderr);
    return stderr;
  }

  FILE *pipe = popen("tmux split-window -h -d -P -F '#{pane_id} #{pane_tty}' 'cat'", "r");
  if (pipe == NULL) {
    perror("tmux split-window");
    return stderr;
  }

  char id[64];
  char tty[PATH_CAP];
  if (fscanf(pipe, "%63s %1023s", id, tty) != 2) {
    pclose(pipe);
    fputs("spore-run: could not read tmux pane tty; using stderr for logs\n", stderr);
    return stderr;
  }
  pclose(pipe);

  FILE *out = fopen(tty, "w");
  if (out == NULL) {
    perror(tty);
    return stderr;
  }
  setvbuf(out, NULL, _IONBF, 0);
  snprintf(pane_id, pane_id_cap, "%s", id);
  return out;
}

static void close_tmux_log_pane(FILE *out, const char *pane_id) {
  if (out != NULL && out != stderr) { fclose(out); }
  if (pane_id != NULL && pane_id[0] != '\0') {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "tmux kill-pane -t '%s' >/dev/null 2>&1", pane_id);
    (void)system(cmd);
  }
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

static int finish_harness(int status, bool raw_terminal, const struct termios *saved_termios, FILE *log_stream,
                          const char *tmux_pane_id) {
  restore_terminal(raw_terminal, saved_termios);
  close_tmux_log_pane(log_stream, tmux_pane_id);
  return status;
}

static pid_t start_udp_echo_server(void) {
  pid_t pid = fork();
  if (pid != 0) { return pid; }
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) { _exit(1); }
  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(5555);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { _exit(1); }
  for (;;) {
    char buf[2048];
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &peer_len);
    if (n > 0) { (void)sendto(fd, buf, (size_t)n, 0, (struct sockaddr *)&peer, peer_len); }
  }
}

static void build_qemu_args(char **argv, int *argc, const char *qemu, const char *image, const char *accel,
                            const char *cpu, const char *vars, char *firmware_arg, size_t firmware_cap, char *vars_arg,
                            size_t vars_cap, char *image_drive_arg, size_t image_drive_cap, int log_fd,
                            char *log_chardev_arg, size_t log_chardev_cap) {
  char firmware[PATH_CAP];
  if (!find_firmware(qemu, firmware, sizeof(firmware))) {
    fputs("spore-run: edk2-aarch64-code.fd not found\n", stderr);
    exit(1);
  }
  ensure_vars_file(qemu, vars);
  snprintf(firmware_arg, firmware_cap, "if=pflash,format=raw,readonly=on,file=%s", firmware);
  if (vars != NULL) { snprintf(vars_arg, vars_cap, "if=pflash,format=raw,file=%s", vars); }
  snprintf(image_drive_arg, image_drive_cap, "if=none,format=raw,readonly=on,file=%s,id=sporeesp", image);
  snprintf(log_chardev_arg, log_chardev_cap, "file,id=sporelog,path=/dev/fd/%d", log_fd);
  int i = 0;
  argv[i++] = (char *)qemu;
  argv[i++] = "-M";
  argv[i++] = "virt,gic-version=3";
  argv[i++] = "-accel";
  argv[i++] = (char *)accel;
  argv[i++] = "-cpu";
  argv[i++] = (char *)cpu;
  argv[i++] = "-m";
  argv[i++] = "512M";
  argv[i++] = "-global";
  argv[i++] = "virtio-mmio.force-legacy=false";
  argv[i++] = "-netdev";
  argv[i++] = "user,id=sporenet";
  argv[i++] = "-device";
  argv[i++] = "virtio-net-device,netdev=sporenet,mac=52:54:00:12:34:56";
  argv[i++] = "-chardev";
  argv[i++] = log_chardev_arg;
  argv[i++] = "-device";
  argv[i++] = "virtio-serial-device";
  argv[i++] = "-device";
  argv[i++] = "virtconsole,chardev=sporelog,name=spore.log";
  argv[i++] = "-boot";
  argv[i++] = "order=d,menu=off,strict=on";
  argv[i++] = "-drive";
  argv[i++] = firmware_arg;
  if (vars != NULL) {
    argv[i++] = "-drive";
    argv[i++] = vars_arg;
  }
  if (ends_with(image, ".iso")) {
    argv[i++] = "-cdrom";
    argv[i++] = (char *)image;
  } else {
    argv[i++] = "-drive";
    argv[i++] = image_drive_arg;
    argv[i++] = "-device";
    argv[i++] = "virtio-blk-device,drive=sporeesp,bootindex=0";
  }
  argv[i++] = "-serial";
  argv[i++] = "stdio";
  argv[i++] = "-display";
  argv[i++] = "none";
  argv[i] = NULL;
  *argc = i;
}

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

struct csi_filter {
  int state;
};

struct startup_serial {
  char buf[STARTUP_SERIAL_CAP];
  size_t len;
  bool done;
};

static void write_sanitized_to(struct csi_filter *filter, const char *data, size_t len, FILE *out) {
  for (size_t i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)data[i];
    if (filter->state == 0) {
      if (c == 0x1b) {
        filter->state = 1;
      } else {
        fwrite(&data[i], 1, 1, out);
      }
    } else if (filter->state == 1) {
      filter->state = c == '[' ? 2 : 0;
    } else {
      if (c >= 0x40 && c <= 0x7e) { filter->state = 0; }
    }
  }
  fflush(out);
}

static void write_raw_to(const char *data, size_t len, FILE *out) {
  fwrite(data, 1, len, out);
  fflush(out);
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

static ssize_t find_shell_prompt(const char *data, size_t len) {
  for (size_t i = 0; i + 3 <= len; ++i) {
    if (data[i] == ' ' && data[i + 1] == '$' && data[i + 2] == ' ') {
      size_t start = i;
      while (start > 0 && data[start - 1] != '\n' && data[start - 1] != '\r') {
        --start;
      }
      return (ssize_t)start;
    }
  }
  return -1;
}

static ssize_t find_after_last_marker(const char *data, size_t len, const char *marker) {
  size_t marker_len = strlen(marker);
  ssize_t found = -1;
  if (marker_len == 0 || len < marker_len) { return -1; }
  for (size_t i = 0; i + marker_len <= len; ++i) {
    if (memcmp(data + i, marker, marker_len) == 0) { found = (ssize_t)(i + marker_len); }
  }
  return found;
}

static void write_serial_output(struct csi_filter *serial_csi, const char *chunk, size_t n, bool tmux_log_pane,
                                struct startup_serial *startup, FILE *log_stream) {
  if (startup->done) {
    write_raw_to(chunk, n, stdout);
    return;
  }

  startup_append(startup, chunk, n);
  ssize_t prompt_at = find_shell_prompt(startup->buf, startup->len);
  if (prompt_at < 0) { return; }

  ssize_t handoff_at = find_after_last_marker(startup->buf, startup->len, "spore-boot: exited boot services");
  if (handoff_at < 0) { handoff_at = prompt_at; }
  while ((size_t)handoff_at < startup->len && (startup->buf[handoff_at] == '\r' || startup->buf[handoff_at] == '\n')) {
    ++handoff_at;
  }

  FILE *startup_out = tmux_log_pane ? log_stream : stdout;
  if (handoff_at > 0) { write_sanitized_to(serial_csi, startup->buf, (size_t)handoff_at, startup_out); }
  write_raw_to(startup->buf + handoff_at, startup->len - (size_t)handoff_at, stdout);
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
  fputs("\n[host] boot timings:\n", out);
  if (first_output_at >= 0.0) { fprintf(out, "[host]   +%.3fs first qemu output\n", first_output_at); }
  for (size_t i = 0; i < count; ++i) {
    if (milestones[i].seen) { fprintf(out, "[host]   +%.3fs %s\n", milestones[i].at, milestones[i].label); }
  }
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

static void print_filtered(const char *buf) {
  const char *p = buf;
  bool printed = false;
  while (*p != '\0') {
    const char *line_end = strchr(p, '\n');
    size_t len = line_end == NULL ? strlen(p) : (size_t)(line_end - p);
    char line[4096];
    size_t copy_len = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, p, copy_len);
    line[copy_len] = '\0';
    if (strstr(line, "[spore]") != NULL || strstr(line, "[cell ") != NULL ||
        strstr(line, "[kernel] lower sync fault") != NULL) {
      fwrite(p, 1, len, stdout);
      fputc('\n', stdout);
      printed = true;
    }
    if (line_end == NULL) { break; }
    p = line_end + 1;
  }
  if (!printed) { fputs(buf, stdout); }
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

static int run_harness(char **qemu_argv, const char *mode, bool timings, bool tmux_log_pane, int log_pipe[2]) {
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
    close(log_pipe[0]);
    execvp(qemu_argv[0], qemu_argv);
    perror(qemu_argv[0]);
    _exit(127);
  }
  close(in_pipe[0]);
  close(serial_pipe[1]);
  close(log_pipe[1]);
  pid_t echo_pid = start_udp_echo_server();

  char buf[BUF_CAP] = {0};
  size_t len = 0;
  size_t sent = 0;
  bool stdin_sent = false;
  bool plain = strcmp(mode, "plain") == 0;
  bool interactive = plain;
  struct termios saved_termios;
  bool raw_terminal = interactive && enter_raw_terminal(&saved_termios);
  double start = now_seconds();
  double deadline = plain ? 0.0 : start + (strcmp(mode, "shell") == 0 ? 75.0 : 30.0);
  bool first_output = false;
  double first_output_at = -1.0;
  bool timing_summary_printed = false;
  struct startup_serial startup_serial = {0};
  char tmux_pane_id[64] = {0};
  FILE *log_stream = tmux_log_pane ? open_tmux_log_pane(tmux_pane_id, sizeof(tmux_pane_id)) : stderr;
  struct csi_filter serial_csi = {0};
  struct csi_filter log_csi = {0};
  struct boot_milestone milestones[] = {
    {"UEFI firmware", "edk2 first banner", false, 0.0},
    {"BdsDxe: starting", "edk2 starts boot option", false, 0.0},
    {"spore-boot: loading", "spore bootloader starts", false, 0.0},
    {"spore-boot: exited boot services", "exit boot services", false, 0.0},
    {"[kernel] booted", "kernel first log", false, 0.0},
    {"[kernel] entering EL0", "enter EL0", false, 0.0},
    {"/ $ ", "shell prompt", false, 0.0},
  };
  if (timings) {
    fprintf(log_stream, "[host] qemu launched; timing summary prints at shell prompt\n");
    fflush(log_stream);
  }
  for (;;) {
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) == pid) {
      char chunk[4096];
      ssize_t n;
      while ((n = read(serial_pipe[0], chunk, sizeof(chunk))) > 0) {
        if (plain || strcmp(mode, "shell") == 0) {
          write_serial_output(&serial_csi, chunk, (size_t)n, tmux_log_pane, &startup_serial, log_stream);
        }
        append(buf, &len, chunk, (size_t)n);
      }
      while ((n = read(log_pipe[0], chunk, sizeof(chunk))) > 0) {
        if (plain || strcmp(mode, "shell") == 0) {
          FILE *out = tmux_log_pane ? log_stream : stdout;
          write_sanitized_to(&log_csi, chunk, (size_t)n, out);
        }
        append(buf, &len, chunk, (size_t)n);
      }
      if (strcmp(mode, "filter") == 0 || strcmp(mode, "stdin") == 0) {
        print_filtered(buf);
      } else if (!plain && strcmp(mode, "shell") != 0) {
        fputs(buf, stdout);
      }
      int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
      if (echo_pid > 0) {
        (void)kill(echo_pid, SIGTERM);
        (void)waitpid(echo_pid, NULL, 0);
      }
      return finish_harness(rc, raw_terminal, &saved_termios, tmux_log_pane ? log_stream : NULL, tmux_pane_id);
    }
    if (!plain && now_seconds() > deadline) {
      kill(pid, SIGTERM);
      waitpid(pid, NULL, 0);
      if (echo_pid > 0) {
        (void)kill(echo_pid, SIGTERM);
        (void)waitpid(echo_pid, NULL, 0);
      }
      return finish_harness(124, raw_terminal, &saved_termios, tmux_log_pane ? log_stream : NULL, tmux_pane_id);
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(serial_pipe[0], &rfds);
    FD_SET(log_pipe[0], &rfds);
    if (interactive) { FD_SET(STDIN_FILENO, &rfds); }
    int max_fd = serial_pipe[0] > log_pipe[0] ? serial_pipe[0] : log_pipe[0];
    if (interactive && STDIN_FILENO > max_fd) { max_fd = STDIN_FILENO; }
    struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
    if (select(max_fd + 1, &rfds, NULL, NULL, &tv) > 0) {
      if (interactive && FD_ISSET(STDIN_FILENO, &rfds)) {
        char input[1024];
        ssize_t n = read(STDIN_FILENO, input, sizeof(input));
        if (n > 0) { (void)write(in_pipe[1], input, (size_t)n); }
      }
      char chunk[1024];
      if (FD_ISSET(serial_pipe[0], &rfds)) {
        ssize_t n = read(serial_pipe[0], chunk, sizeof(chunk));
        if (n > 0) {
          record_output(buf, &len, chunk, (size_t)n, milestones, sizeof(milestones) / sizeof(milestones[0]), timings,
                        start, &first_output, &first_output_at, &timing_summary_printed, log_stream);
          if (plain || strcmp(mode, "shell") == 0) {
            write_serial_output(&serial_csi, chunk, (size_t)n, tmux_log_pane, &startup_serial, log_stream);
          }
        }
      }
      if (FD_ISSET(log_pipe[0], &rfds)) {
        ssize_t n = read(log_pipe[0], chunk, sizeof(chunk));
        if (n > 0) {
          record_output(buf, &len, chunk, (size_t)n, milestones, sizeof(milestones) / sizeof(milestones[0]), timings,
                        start, &first_output, &first_output_at, &timing_summary_printed, log_stream);
          if (plain || strcmp(mode, "shell") == 0) {
            FILE *out = tmux_log_pane ? log_stream : stdout;
            write_sanitized_to(&log_csi, chunk, (size_t)n, out);
          }
        }
      }
      if (len > 0) {
        if (strcmp(mode, "stdin") == 0 && !stdin_sent &&
            contains(buf, "[spore] stdin demo: child blocking on read(0)")) {
          write(in_pipe[1], "z\n", 2);
          stdin_sent = true;
        }
        if (strcmp(mode, "shell") == 0 && sent < sizeof(shell_commands) / sizeof(shell_commands[0]) &&
            contains(buf, " $ ")) {
          write(in_pipe[1], shell_commands[sent], strlen(shell_commands[sent]));
          ++sent;
          buf[0] = '\0';
          len = 0;
        }
      }
    }
  }
}

int main(int argc, char **argv) {
  const char *mode = "filter";
  const char *image = NULL;
  const char *qemu = "qemu-system-aarch64";
  const char *accel = "hvf";
  const char *cpu = "host";
  const char *vars = NULL;
  bool timings = false;
  bool tmux_log_pane = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      mode = argv[++i];
    } else if (strcmp(argv[i], "--timings") == 0) {
      timings = true;
    } else if (strcmp(argv[i], "--tmux-log-pane") == 0) {
      tmux_log_pane = true;
    } else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
      image = argv[++i];
    } else if (strcmp(argv[i], "--qemu") == 0 && i + 1 < argc) {
      qemu = argv[++i];
    } else if (strcmp(argv[i], "--accel") == 0 && i + 1 < argc) {
      accel = argv[++i];
    } else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
      cpu = argv[++i];
    } else if (strcmp(argv[i], "--vars") == 0 && i + 1 < argc) {
      vars = argv[++i];
    } else {
      usage();
    }
  }
  if (image == NULL) { usage(); }
  char firmware_arg[PATH_CAP];
  char vars_arg[PATH_CAP];
  char image_drive_arg[PATH_CAP];
  char log_chardev_arg[PATH_CAP];
  int log_pipe[2];
  if (pipe(log_pipe) != 0) {
    perror("pipe");
    return 1;
  }
  char *qemu_argv[80];
  int qemu_argc = 0;
  build_qemu_args(qemu_argv, &qemu_argc, qemu, image, accel, cpu, vars, firmware_arg, sizeof(firmware_arg), vars_arg,
                  sizeof(vars_arg), image_drive_arg, sizeof(image_drive_arg), log_pipe[1], log_chardev_arg,
                  sizeof(log_chardev_arg));
  (void)qemu_argc;
  if (strcmp(mode, "plain") == 0 || strcmp(mode, "filter") == 0 || strcmp(mode, "shell") == 0 ||
      strcmp(mode, "stdin") == 0) {
    return run_harness(qemu_argv, mode, timings, tmux_log_pane, log_pipe);
  }
  usage();
  return 2;
}
