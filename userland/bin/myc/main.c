#include <errno.h>
#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int send_command(int argc, char **argv) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("myc: socket");
    return EXIT_FAILURE;
  }
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof(sa.sun_path), "/run/mycelium.sock");
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    perror("myc: connect /run/mycelium.sock");
    close(fd);
    return EXIT_FAILURE;
  }

  for (int i = 0; i < argc; ++i) {
    if (i > 0) { (void)write(fd, " ", 1); }
    (void)write(fd, argv[i], strlen(argv[i]));
  }
  (void)write(fd, "\n", 1);

  char buf[512];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) { continue; }
    if (n <= 0) { break; }
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }
  close(fd);
  return EXIT_SUCCESS;
}

static int logs(int argc, char **argv) {
  bool follow = argc > 1 && streq(argv[1], "-f");
  int index = follow ? 2 : 1;
  if (argc > index + 1) {
    fputs("usage: myc logs [-f] [UNIT]\n", stderr);
    return EXIT_USAGE;
  }
  char path[128] = "/var/log/syslog";
  if (argc > index) {
    const char *name = argv[index];
    if (!*name || strchr(name, '/')) {
      fputs("logs: invalid unit name\n", stderr);
      return EXIT_FAILURE;
    }
    int n = snprintf(path, sizeof(path), "/var/log/%s%s.log", name, strchr(name, '.') ? "" : ".service");
    if (n < 0 || (size_t)n >= sizeof(path)) { return EXIT_USAGE; }
    if (access(path, R_OK) < 0 && errno == ENOENT && !follow) { return send_command(argc, argv); }
  }
  if (follow) {
    execlp("tail", "tail", "-n", "50", "-f", path, (char *)NULL);
  } else {
    execlp("tail", "tail", "-n", "50", path, (char *)NULL);
  }
  perror("myc: tail");
  return EXIT_FAILURE;
}

int main(int argc, char **argv) {
  if (argc < 2 || streq(argv[1], "--help")) {
    puts("usage: myc COMMAND [UNIT]");
    puts("commands: start stop restart reload status show cat list-units list-timers");
    puts("          list-dependencies is-active is-failed daemon-reload logs isolate reboot poweroff");
    return argc < 2 ? EXIT_USAGE : EXIT_SUCCESS;
  }
  if (streq(argv[1], "logs")) { return logs(argc - 1, argv + 1); }
  return send_command(argc - 1, argv + 1);
}
