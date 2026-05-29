#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
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

static volatile sig_atomic_t resize_pending;

enum scripted_input {
  SCRIPT_NONE,
  SCRIPT_EDIT_MOTD,
  SCRIPT_EDIT_PASTE,
  SCRIPT_PICO_SIMPLE,
  SCRIPT_PICO_PASTE,
  SCRIPT_NANO_PASTE,
  SCRIPT_ANT_CTRL_C,
};

static const char *shell_commands[] = {
  "ls /bin\n",
  "ls /dev\n",
  "ls /dev/fs\n",
  "ls /dev/blk\n",
  "ls /proc\n",
  "cat /proc/procinfo\n",
  "cat /proc/meminfo\n",
  "cat /proc/mounts\n",
  "cat /proc/filesystems\n",
  "cat /proc/partitions\n",
  "cat /proc/devices\n",
  "stat /dev/null /dev/blk/root /proc/mounts /tmp\n",
  "echo null-ok > /dev/null\n",
  "echo console-ok > /dev/console\n",
  "echo full > /dev/full\n",
  "cat /etc/motd\n",
  "uname -a\n",
  "myc status\n",
  "myc list-units\n",
  "myc list-dependencies multi-user.target\n",
  "myc status network.service\n",
  "cat /etc/resolv.conf\n",
  "hostname\n",
  "hostname -s\n",
  "hostname -d\n",
  "hostname -i\n",
  "hostname -I\n",
  "ping -c 1 127.0.0.1\n",
  "nslookup localhost\n",
  "nslookup example.com\n",
  "printf '10.0.2.2 example.test\\n' | sudo tee -a /etc/hosts\n",
  "curl http://example.test:8080/\n",
  "confine net:none curl http://example.test:8080/\n",
  "confine net:tcp:10.0.2.2:8080 curl http://example.test:8080/\n",
  "confine net:dns nslookup example.com\n",
  "myc list-timers\n",
  "/bin/dash -c 'echo dash-ok; echo $((1 + 2)); cd /tmp && pwd'\n",
  "/bin/dash\n",
  "ls\n",
  "exit\n",
  "msh -c 'echo msh-c-ok'\n",
  "__spore_harness_msh_paste__\n",
  "wc /tmp/msh-paste\n",
  "grep msh-paste-064 /tmp/msh-paste\n",
  "echo '#!/usr/bin/env msh' > /tmp/script.msh\n",
  "echo 'if true; then' >> /tmp/script.msh\n",
  "echo ' echo if-ok' >> /tmp/script.msh\n",
  "echo 'else' >> /tmp/script.msh\n",
  "echo ' echo if-bad' >> /tmp/script.msh\n",
  "echo 'fi' >> /tmp/script.msh\n",
  "echo 'while false; do' >> /tmp/script.msh\n",
  "echo ' echo loop-bad' >> /tmp/script.msh\n",
  "echo 'done' >> /tmp/script.msh\n",
  "echo 'echo script-ok' >> /tmp/script.msh\n",
  "chmod 755 /tmp/script.msh\n",
  "/tmp/script.msh\n",
  "echo one > /tmp/coreutils\n",
  "echo two >> /tmp/coreutils\n",
  "head -n 1 /tmp/coreutils\n",
  "tail -n 1 /tmp/coreutils\n",
  "wc /tmp/coreutils\n",
  "grep -n two /tmp/coreutils\n",
  "find /etc\n",
  "echo b > /tmp/sortin && echo a >> /tmp/sortin && sort /tmp/sortin\n",
  "echo a > /tmp/uniqin && echo a >> /tmp/uniqin && uniq -c /tmp/uniqin\n",
  "echo aa:bb > /tmp/cutin && cut -d : -f 2 /tmp/cutin\n",
  "echo abc > /tmp/trin && tr a-z A-Z < /tmp/trin\n",
  "echo hello | tr a-z A-Z | cat\n",
  "printf 'left\\nright\\n' | grep right | wc\n",
  "mkfifo /run/p\n",
  "cat /run/p &\n",
  "echo via-fifo > /run/p\n",
  "wait\n",
  "ls -l /run\n",
  "/libexec/spore-test/uds-server /run/echo.sock &\n",
  "/libexec/spore-test/uds-client /run/echo.sock ping\n",
  "wait\n",
  "ls -l /run\n",
  "echo one two > /tmp/sedin && sed-lite 's/two/three/' /tmp/sedin\n",
  "tar -cf /tmp/coreutils.tar /tmp/coreutils\n",
  "tar -tf /tmp/coreutils.tar\n",
  "ps\n",
  "top -b\n",
  "sleep 1 &\n",
  "jobs\n",
  "cat /proc/1/status\n",
  "cat /proc/1/cmdline\n",
  "cat /proc/1/exe\n",
  "wait\n",
  "jobs\n",
  "/home/spore/demos/signal-crash all\n",
  "/home/spore/demos/signal-crash sleep &\n",
  "jobs\n",
  "kill -9 %1\n",
  "wait %1\n",
  "df\n",
  "free\n",
  "du -sh /tmp\n",
  "dirname /tmp/coreutils\n",
  "basename /tmp/coreutils\n",
  "date\n",
  "uptime\n",
  "whoami\n",
  "hostname\n",
  "hostid\n",
  "id\n",
  "who\n",
  "tty\n",
  "env FOO=bar printenv FOO\n",
  "printf 'abc\\n' > /tmp/hashin\n",
  "base64 /tmp/hashin > /tmp/hashin.b64\n",
  "base64 -d /tmp/hashin.b64\n",
  "cksum /tmp/hashin\n",
  "sum /tmp/hashin\n",
  "sha1sum /tmp/hashin\n",
  "du -s /tmp\n",
  "readlink -f /tmp/../tmp/hashin\n",
  "time true\n",
  "printf 'all:\\n\\techo mk-ok\\n' > /tmp/mkfile\n",
  "mk -n -f /tmp/mkfile all\n",
  "chroot / /bin/pwd\n",
  "cp /tmp/coreutils /tmp/coreutils.copy\n",
  "mv /tmp/coreutils.copy /tmp/coreutils.moved\n",
  "ln /tmp/coreutils /tmp/coreutils.link\n",
  "chmod 600 /tmp/coreutils\n",
  "stat /tmp/coreutils\n",
  "cat /tmp/coreutils.link\n",
  "tee /tmp/coreutils.tee < /tmp/coreutils\n",
  "cat /tmp/coreutils.tee\n",
  "printf '{\"spore\":true}\\n' | jq .\n",
  "printf 'one\\ntwo\\n' | more\n",
  "printf 'one\\ntwo\\n' | less\n",
  "hexdump /tmp/coreutils\n",
  "xxd /tmp/coreutils\n",
  "sudo edit /etc/motd\n",
  "cat /etc/motd\n",
  "echo hi > /tmp/f\n",
  "cat /tmp/f\n",
  "edit /tmp/edit-test\na\nfrom edit\n.\nw\nq\n",
  "cat /tmp/edit-test\n",
  "edit /tmp/edit-paste\n",
  "wc /tmp/edit-paste\n",
  "grep edit-paste-064 /tmp/edit-paste\n",
  "pico /tmp/pico-test\n",
  "cat /tmp/pico-test\n",
  "pico /tmp/pico-paste\n",
  "wc /tmp/pico-paste\n",
  "grep pico-paste-032 /tmp/pico-paste\n",
  "nano /tmp/nano-paste\n",
  "wc /tmp/nano-paste\n",
  "grep nano-paste-032 /tmp/nano-paste\n",
  "ant\n",
  "mkdir /tmp/d && cd /tmp/d && touch x && ls\n",
  "/bin/hello\n",
  "/libexec/spore-test/pthread-demo\n",
  "ping -c 1 10.0.2.2\n",
  "/libexec/spore-test/udp-echo 10.0.2.2 5555 hi\n",
  "confine net:none /libexec/spore-test/udp-send 10.0.2.2 5555 hi\n",
  "confine net:udp:10.0.2.2:5555 /libexec/spore-test/udp-send 10.0.2.2 5555 hi\n",
  "confine compute-only /home/spore/demos/spinner\n",
  "confine fs:/tmp /home/spore/demos/peeker /etc/motd\n",
  "confine fs:/tmp /home/spore/demos/writer /tmp/d/out\n",
  "confine mem:1 /home/spore/demos/memhog\n",
  "runc bad-manifest /home/spore/demos/escalate\n",
  "sudo shutdown\n",
};

struct bench_command {
  const char *label;
  const char *command;
};

static const struct bench_command bench_commands[] = {
  {"ant startup", "time ant -e 'console.log(1)' > /dev/null\n"},
  {"ls /bin", "time ls /bin > /dev/null\n"},
  {"du -s /", "time du -s / > /dev/null\n"},
  {"find /etc", "time find /etc > /dev/null\n"},
  {"ls -l usr/local", "time ls -l /usr/local/bin > /dev/null\n"},
  {"dynamic opens", "time curl -V > /dev/null\n"},
  {"curl http", "time curl -s http://10.0.2.2:8080/ > /dev/null\n"},
  {"nano --version", "time nano --version > /dev/null\n"},
  {"fs stats", "cat /proc/fsstats\n"},
};

static void usage(void) {
  fputs("usage: spore-run [--mode plain|filter|shell|stdin|bench|rng] [--timings] [--log-to-stderr] --image IMAGE "
        "[--root ROOT_EXT2] [--qemu QEMU] [--accel ACCEL] [--cpu CPU] [--memory MEM] [--smp N] [--vars VARS_FD]\n",
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

static void write_serial_input(int fd, const char *buf, size_t len) {
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

static size_t append_format(char *buf, size_t cap, size_t len, const char *fmt, ...) {
  if (len >= cap) { return len; }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
  if (n <= 0) { return len; }
  size_t add = (size_t)n;
  if (add > cap - len) { return cap; }
  return len + add;
}

static void send_numbered_lines(int fd, const char *prefix, const char *tag, size_t count, const char *suffix) {
  char input[16384];
  size_t len = 0;
  if (prefix != NULL) { len = append_format(input, sizeof(input), len, "%s", prefix); }
  for (size_t i = 1; i <= count; ++i) {
    len = append_format(input, sizeof(input), len, "%s-%03zu\n", tag, i);
  }
  if (suffix != NULL) { len = append_format(input, sizeof(input), len, "%s", suffix); }
  if (len > sizeof(input)) { len = sizeof(input); }
  write_serial_input(fd, input, len);
}

static void send_numbered_cr_lines(int fd, const char *tag, size_t count, const char *suffix) {
  char input[16384];
  size_t len = 0;
  for (size_t i = 1; i <= count; ++i) {
    len = append_format(input, sizeof(input), len, "%s-%03zu\r", tag, i);
  }
  if (suffix != NULL) { len = append_format(input, sizeof(input), len, "%s", suffix); }
  if (len > sizeof(input)) { len = sizeof(input); }
  write_serial_input(fd, input, len);
}

static void send_shell_printf_paste(int fd) {
  char input[4096];
  size_t len = append_format(input, sizeof(input), 0, "printf '");
  for (size_t i = 1; i <= 64; ++i) {
    len = append_format(input, sizeof(input), len, "msh-paste-%03zu\\n", i);
  }
  len = append_format(input, sizeof(input), len, "' > /tmp/msh-paste\n");
  if (len > sizeof(input)) { len = sizeof(input); }
  write_serial_input(fd, input, len);
}

static void on_sigwinch(int sig) {
  (void)sig;
  resize_pending = 1;
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

static pid_t start_http_server(void) {
  pid_t pid = fork();
  if (pid != 0) { return pid; }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { _exit(1); }
  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(8080);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) { _exit(1); }
  for (;;) {
    int client = accept(fd, NULL, NULL);
    if (client < 0) { continue; }
    char buf[1024];
    (void)read(client, buf, sizeof(buf));
    const char body[] = "hello from host http\n";
    const char header[] =
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 21\r\nConnection: close\r\n\r\n";
    (void)write(client, header, sizeof(header) - 1);
    (void)write(client, body, sizeof(body) - 1);
    close(client);
  }
}

static void build_qemu_args(char **argv, int *argc, const char *qemu, const char *image, const char *root,
                            const char *accel, const char *cpu, const char *memory, const char *smp, const char *vars,
                            char *firmware_arg, size_t firmware_cap, char *vars_arg, size_t vars_cap,
                            char *image_drive_arg, size_t image_drive_cap, char *root_drive_arg, size_t root_drive_cap,
                            int log_fd, char *log_chardev_arg, size_t log_chardev_cap) {
  char firmware[PATH_CAP];
  if (!find_firmware(qemu, firmware, sizeof(firmware))) {
    fputs("spore-run: edk2-aarch64-code.fd not found\n", stderr);
    exit(1);
  }
  ensure_vars_file(qemu, vars);
  snprintf(firmware_arg, firmware_cap, "if=pflash,format=raw,readonly=on,file=%s", firmware);
  if (vars != NULL) { snprintf(vars_arg, vars_cap, "if=pflash,format=raw,file=%s", vars); }
  snprintf(image_drive_arg, image_drive_cap, "if=none,format=raw,readonly=on,file=%s,id=sporeesp", image);
  if (root != NULL) { snprintf(root_drive_arg, root_drive_cap, "if=none,format=raw,file=%s,id=sporeroot", root); }
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
  argv[i++] = (char *)memory;
  argv[i++] = "-smp";
  argv[i++] = (char *)smp;
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
  if (root != NULL) {
    argv[i++] = "-drive";
    argv[i++] = root_drive_arg;
    argv[i++] = "-device";
    argv[i++] = "virtio-blk-device,drive=sporeroot";
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

static ssize_t find_shell_prompt(const char *data, size_t len) {
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
      if (old_prompt || user_host_prompt) { return (ssize_t)start; }
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

static void write_serial_output(const char *chunk, size_t n, struct startup_serial *startup) {
  if (startup->done) {
    write_raw_to(chunk, n, stdout);
    return;
  }

  startup_append(startup, chunk, n);
  ssize_t prompt_at = find_shell_prompt(startup->buf, startup->len);
  if (prompt_at < 0) { return; }

  const char *boot_ui_marker = "\033[1;36m       .-.";
  ssize_t handoff_at = find_after_last_marker(startup->buf, startup->len, boot_ui_marker);
  if (handoff_at >= 0) {
    handoff_at -= (ssize_t)strlen(boot_ui_marker);
  } else {
    const char *mycelium_marker = "spore: mycelium starting";
    handoff_at = find_after_last_marker(startup->buf, startup->len, mycelium_marker);
    if (handoff_at >= 0) {
      handoff_at -= (ssize_t)strlen(mycelium_marker);
    } else {
      handoff_at = prompt_at;
    }
  }
  while ((size_t)handoff_at < startup->len && (startup->buf[handoff_at] == '\r' || startup->buf[handoff_at] == '\n')) {
    ++handoff_at;
  }

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

static bool capture_xxd_line(const char *buf, char *out, size_t cap) {
  const char *line = strstr(buf, "00000000:");
  if (line == NULL || cap == 0) { return false; }
  const char *end = strchr(line, '\n');
  size_t len = end == NULL ? strlen(line) : (size_t)(end - line);
  if (len >= cap) { len = cap - 1; }
  memcpy(out, line, len);
  out[len] = '\0';
  return true;
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

static int run_harness(char **qemu_argv, const char *mode, bool timings, bool mirror_log, int log_pipe[2]) {
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
  pid_t http_pid = start_http_server();
  resize_pending = 1;
  (void)signal(SIGWINCH, on_sigwinch);

  char buf[BUF_CAP] = {0};
  size_t len = 0;
  size_t sent = 0;
  bool stdin_sent = false;
  bool shell_size_sent = false;
  enum scripted_input scripted_input = SCRIPT_NONE;
  int ant_ctrl_c_stage = 0;
  bool plain = strcmp(mode, "plain") == 0;
  bool shell = strcmp(mode, "shell") == 0;
  bool bench = strcmp(mode, "bench") == 0;
  bool rng = strcmp(mode, "rng") == 0;
  bool interactive = plain;
  size_t bench_done = 0;
  bool bench_waiting = false;
  bool bench_shutdown_sent = false;
  double bench_started_at = 0.0;
  int rng_stage = 0;
  char rng_sample[256] = {0};
  struct termios saved_termios;
  bool raw_terminal = interactive && enter_raw_terminal(&saved_termios);
  double start = now_seconds();
  double deadline = plain ? 0.0 : start + 120.0;
  bool first_output = false;
  double first_output_at = -1.0;
  bool timing_summary_printed = false;
  struct startup_serial startup_serial = {0};
  FILE *log_stream = stderr;
  struct boot_milestone milestones[] = {
    {"UEFI firmware", "edk2 first banner", false, 0.0},
    {"BdsDxe: starting", "edk2 starts boot option", false, 0.0},
    {"spore-boot: loading", "spore bootloader starts", false, 0.0},
    {"spore-boot: exited boot services", "exit boot services", false, 0.0},
    {"[kernel] booted", "kernel first log", false, 0.0},
    {"[kernel] entering EL0", "enter EL0", false, 0.0},
    {"$ ", "shell prompt", false, 0.0},
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
        if (plain || shell) { write_serial_output(chunk, (size_t)n, &startup_serial); }
        append(buf, &len, chunk, (size_t)n);
      }
      while ((n = read(log_pipe[0], chunk, sizeof(chunk))) > 0) {
        if (mirror_log) { write_raw_to(chunk, (size_t)n, stderr); }
        append(buf, &len, chunk, (size_t)n);
      }
      if (rng && rng_sample[0] != '\0') { printf("[rng] %s\n", rng_sample); }
      if (strcmp(mode, "filter") == 0 || strcmp(mode, "stdin") == 0) {
        print_filtered(buf);
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
    if (resize_pending) {
      resize_pending = 0;
      send_terminal_size(in_pipe[1]);
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
        if (n > 0) { write_serial_input(in_pipe[1], input, (size_t)n); }
      }
      char chunk[1024];
      if (FD_ISSET(serial_pipe[0], &rfds)) {
        ssize_t n = read(serial_pipe[0], chunk, sizeof(chunk));
        if (n > 0) {
          record_output(buf, &len, chunk, (size_t)n, milestones, sizeof(milestones) / sizeof(milestones[0]), timings,
                        start, &first_output, &first_output_at, &timing_summary_printed, log_stream);
          if (plain || shell) { write_serial_output(chunk, (size_t)n, &startup_serial); }
        }
      }
      if (FD_ISSET(log_pipe[0], &rfds)) {
        ssize_t n = read(log_pipe[0], chunk, sizeof(chunk));
        if (n > 0) {
          if (mirror_log) { write_raw_to(chunk, (size_t)n, stderr); }
          record_output(buf, &len, chunk, (size_t)n, milestones, sizeof(milestones) / sizeof(milestones[0]), timings,
                        start, &first_output, &first_output_at, &timing_summary_printed, log_stream);
        }
      }
      if (len > 0) {
        if (!shell_size_sent && find_shell_prompt(buf, len) >= 0) {
          send_terminal_size(in_pipe[1]);
          shell_size_sent = true;
        }
        if (strcmp(mode, "stdin") == 0 && !stdin_sent &&
            contains(buf, "[spore] stdin demo: child blocking on read(0)")) {
          write_serial_input(in_pipe[1], "z\n", 2);
          stdin_sent = true;
        }
        if (shell && scripted_input == SCRIPT_PICO_SIMPLE && contains(buf, "Spore Pico")) {
          static const char pico_input[] = "from pico\x0f\x18\x18";
          write_serial_input(in_pipe[1], pico_input, sizeof(pico_input) - 1);
          scripted_input = SCRIPT_NONE;
          buf[0] = '\0';
          len = 0;
        }
        if (shell && scripted_input == SCRIPT_PICO_PASTE && contains(buf, "Spore Pico")) {
          send_numbered_lines(in_pipe[1], NULL, "pico-paste", 32, "\x0f\x18\x18");
          scripted_input = SCRIPT_NONE;
          buf[0] = '\0';
          len = 0;
        }
        if (shell && scripted_input == SCRIPT_NANO_PASTE && contains(buf, "GNU nano")) {
          send_numbered_cr_lines(in_pipe[1], "nano-paste", 32, "\x0f\r\x18");
          scripted_input = SCRIPT_NONE;
          buf[0] = '\0';
          len = 0;
        }
        if (shell && scripted_input == SCRIPT_EDIT_MOTD && contains(buf, "edit>")) {
          static const char edit_input[] = "d 1\na\nshell-check motd\n.\nw\nq\n";
          write_serial_input(in_pipe[1], edit_input, sizeof(edit_input) - 1);
          scripted_input = SCRIPT_NONE;
          buf[0] = '\0';
          len = 0;
        }
        if (shell && scripted_input == SCRIPT_EDIT_PASTE && contains(buf, "edit>")) {
          send_numbered_lines(in_pipe[1], "a\n", "edit-paste", 64, ".\nw\nq\n");
          scripted_input = SCRIPT_NONE;
          buf[0] = '\0';
          len = 0;
        }
        if (shell && scripted_input == SCRIPT_ANT_CTRL_C && ant_ctrl_c_stage == 0 &&
            contains(buf, "for more information")) {
          static const char ctrl_c[] = "\x03";
          static const char exit_ant[] = ".exit\n";
          write_serial_input(in_pipe[1], ctrl_c, sizeof(ctrl_c) - 1);
          struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000};
          (void)nanosleep(&ts, NULL);
          write_serial_input(in_pipe[1], exit_ant, sizeof(exit_ant) - 1);
          ant_ctrl_c_stage = 2;
          buf[0] = '\0';
          len = 0;
        }
        ssize_t prompt = find_shell_prompt(buf, len);
        if (bench && prompt >= 0) {
          if (bench_waiting) {
            double elapsed = now_seconds() - bench_started_at;
            printf("[bench] %-18s %.3fs\n", bench_commands[bench_done].label, elapsed);
            fflush(stdout);
            ++bench_done;
            bench_waiting = false;
          }
          if (bench_done < sizeof(bench_commands) / sizeof(bench_commands[0])) {
            printf("[bench] running %-18s\n", bench_commands[bench_done].label);
            fflush(stdout);
            bench_started_at = now_seconds();
            write_serial_input(in_pipe[1], bench_commands[bench_done].command,
                               strlen(bench_commands[bench_done].command));
            bench_waiting = true;
          } else if (!bench_shutdown_sent) {
            write_serial_input(in_pipe[1], "sudo shutdown\n", strlen("sudo shutdown\n"));
            bench_shutdown_sent = true;
          }
          buf[0] = '\0';
          len = 0;
        }
        if (rng && prompt >= 0) {
          if (rng_stage == 0) {
            write_serial_input(in_pipe[1], "xxd -l 32 /dev/urandom\n", strlen("xxd -l 32 /dev/urandom\n"));
            rng_stage = 1;
          } else if (rng_stage == 1) {
            (void)capture_xxd_line(buf, rng_sample, sizeof(rng_sample));
            write_serial_input(in_pipe[1], "sudo shutdown\n", strlen("sudo shutdown\n"));
            rng_stage = 2;
          }
          buf[0] = '\0';
          len = 0;
        }
        if (shell && sent < sizeof(shell_commands) / sizeof(shell_commands[0]) && prompt >= 0) {
          if (scripted_input == SCRIPT_ANT_CTRL_C && ant_ctrl_c_stage > 0) {
            scripted_input = SCRIPT_NONE;
            ant_ctrl_c_stage = 0;
          }
          if (strcmp(shell_commands[sent], "__spore_harness_msh_paste__\n") == 0) {
            send_shell_printf_paste(in_pipe[1]);
            ++sent;
            buf[0] = '\0';
            len = 0;
            continue;
          }
          if (strcmp(shell_commands[sent], "sudo edit /etc/motd\n") == 0) { scripted_input = SCRIPT_EDIT_MOTD; }
          if (strcmp(shell_commands[sent], "edit /tmp/edit-paste\n") == 0) { scripted_input = SCRIPT_EDIT_PASTE; }
          if (strcmp(shell_commands[sent], "pico /tmp/pico-test\n") == 0) { scripted_input = SCRIPT_PICO_SIMPLE; }
          if (strcmp(shell_commands[sent], "pico /tmp/pico-paste\n") == 0) { scripted_input = SCRIPT_PICO_PASTE; }
          if (strcmp(shell_commands[sent], "nano /tmp/nano-paste\n") == 0) { scripted_input = SCRIPT_NANO_PASTE; }
          if (strcmp(shell_commands[sent], "ant\n") == 0) {
            scripted_input = SCRIPT_ANT_CTRL_C;
            ant_ctrl_c_stage = 0;
          }
          write_serial_input(in_pipe[1], shell_commands[sent], strlen(shell_commands[sent]));
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
  const char *root = NULL;
  const char *qemu = "qemu-system-aarch64";
  const char *accel = "hvf";
  const char *cpu = "host";
  const char *memory = "8G";
  const char *smp = "4";
  const char *vars = NULL;
  bool timings = false;
  bool mirror_log = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      mode = argv[++i];
    } else if (strcmp(argv[i], "--timings") == 0) {
      timings = true;
    } else if (strcmp(argv[i], "--log-to-stderr") == 0) {
      mirror_log = true;
    } else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) {
      image = argv[++i];
    } else if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
      root = argv[++i];
    } else if (strcmp(argv[i], "--qemu") == 0 && i + 1 < argc) {
      qemu = argv[++i];
    } else if (strcmp(argv[i], "--accel") == 0 && i + 1 < argc) {
      accel = argv[++i];
    } else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
      cpu = argv[++i];
    } else if (strcmp(argv[i], "--memory") == 0 && i + 1 < argc) {
      memory = argv[++i];
    } else if (strcmp(argv[i], "--smp") == 0 && i + 1 < argc) {
      smp = argv[++i];
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
  char root_drive_arg[PATH_CAP];
  char log_chardev_arg[PATH_CAP];
  int log_pipe[2];
  if (pipe(log_pipe) != 0) {
    perror("pipe");
    return 1;
  }
  char *qemu_argv[96];
  int qemu_argc = 0;
  build_qemu_args(qemu_argv, &qemu_argc, qemu, image, root, accel, cpu, memory, smp, vars, firmware_arg,
                  sizeof(firmware_arg), vars_arg, sizeof(vars_arg), image_drive_arg, sizeof(image_drive_arg),
                  root_drive_arg, sizeof(root_drive_arg), log_pipe[1], log_chardev_arg, sizeof(log_chardev_arg));
  (void)qemu_argc;
  if (strcmp(mode, "plain") == 0 || strcmp(mode, "filter") == 0 || strcmp(mode, "shell") == 0 ||
      strcmp(mode, "stdin") == 0 || strcmp(mode, "bench") == 0 || strcmp(mode, "rng") == 0) {
    return run_harness(qemu_argv, mode, timings, mirror_log, log_pipe);
  }
  usage();
  return 2;
}
