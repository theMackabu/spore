#include "integration.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int fifo_without_reader(void) {
  for (int i = 0; i < 32; ++i) {
    char path[32];
    snprintf(path, sizeof(path), "fifo-%d", i);
    if (mkfifo(path, 0600) != 0) { return 0; }
    int writer = open(path, O_WRONLY | O_NONBLOCK);
    if (writer >= 0) {
      close(writer);
      return 0;
    }
    if (errno != ENXIO) { return 0; }
  }
  int reader = open("fifo-0", O_RDONLY | O_NONBLOCK);
  int writer = open("fifo-0", O_WRONLY | O_NONBLOCK);
  char byte;
  int ok = reader >= 0 && writer >= 0 && write(writer, "x", 1) == 1 && read(reader, &byte, 1) == 1 && byte == 'x';
  if (writer >= 0) { close(writer); }
  if (reader >= 0) { close(reader); }
  for (int i = 0; i < 32; ++i) {
    char path[32];
    snprintf(path, sizeof(path), "fifo-%d", i);
    unlink(path);
  }
  return ok;
}

static int disk_fifo(void) {
  const char *name = "/spore-fifo", *link_name = "/spore-fifo-link";
  if (mkfifo(name, 0600) || link(name, link_name)) { return 0; }
  struct stat named, opened;
  int rd = open(name, O_RDONLY | O_NONBLOCK);
  char c;
  int ok = rd >= 0 && read(rd, &c, 1) == 0 && fstat(rd, &opened) == 0 && stat(link_name, &named) == 0 &&
           S_ISFIFO(named.st_mode) && named.st_ino == opened.st_ino && named.st_dev == opened.st_dev;
  int both = open(link_name, O_RDWR | O_NONBLOCK);
  ok = ok && both >= 0 && unlink(name) == 0 && unlink(link_name) == 0 && write(both, "ab", 2) == 2 &&
       read(rd, &c, 1) == 1 && c == 'a' && read(both, &c, 1) == 1 && c == 'b' &&
       fstat(rd, &opened) == 0 && opened.st_nlink == 0;
  if (both >= 0) { close(both); }
  struct pollfd p = {.fd = rd, .events = POLLIN};
  ok = ok && poll(&p, 1, 0) == 1 && (p.revents & POLLHUP) && read(rd, &c, 1) == 0;
  if (rd >= 0) { close(rd); }
  printf("[spore] ext2 FIFO aliases and read/write endpoints: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int fifo_rendezvous(const char *name, int child_mode) {
  int ready[2];
  if (mkfifo(name, 0600) || pipe(ready)) { return 0; }
  pid_t child = fork();
  if (child == 0) {
    close(ready[0]);
    if (write(ready[1], "r", 1) != 1) { _exit(1); }
    int fd = open(name, child_mode | O_CLOEXEC);
    char c;
    int ok = fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC) && write(ready[1], "o", 1) == 1;
    if (ok && child_mode == O_RDONLY) { ok = read(fd, &c, 1) == 1 && c == 'x'; }
    if (ok && child_mode == O_WRONLY) { ok = write(fd, "x", 1) == 1; }
    _exit(ok ? 0 : 1);
  }
  close(ready[1]);
  char c;
  struct pollfd p = {.fd = ready[0], .events = POLLIN};
  int ok = child > 0 && read(ready[0], &c, 1) == 1 && poll(&p, 1, 40) == 0;
  int fd = child > 0 ? open(name, child_mode == O_RDONLY ? O_WRONLY : O_RDONLY) : -1;
  ok = ok && fd >= 0 && read(ready[0], &c, 1) == 1 && c == 'o';
  if (fd >= 0) {
    if (child_mode == O_RDONLY) { ok = write(fd, "x", 1) == 1 && ok; }
    else { ok = read(fd, &c, 1) == 1 && c == 'x' && ok; }
    close(fd);
  }
  close(ready[0]);
  int status;
  if (child > 0) { ok = waitpid(child, &status, 0) == child && status == 0 && ok; }
  return unlink(name) == 0 && ok;
}

static void alarm_handler(int signo) { (void)signo; }

static int interrupted_open(void) {
  const char *name = "/tmp/interrupted-fifo";
  if (mkfifo(name, 0600)) { return 0; }
  pid_t child = fork();
  if (child == 0) {
    struct sigaction act = {.sa_handler = alarm_handler};
    struct itimerval timer = {.it_value = {.tv_usec = 40000}};
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGALRM, &act, NULL) || setitimer(ITIMER_REAL, &timer, NULL)) { _exit(1); }
    _exit(open(name, O_RDONLY) == -1 && errno == EINTR &&
          open(name, O_NONBLOCK | O_WRONLY) == -1 && errno == ENXIO ? 0 : 1);
  }
  int status;
  int ok = child > 0 && waitpid(child, &status, 0) == child && status == 0;
  return unlink(name) == 0 && ok;
}

int integration_fifo_suite(void) {
  int ok = fifo_without_reader() && disk_fifo() && fifo_rendezvous("/fifo-wait", O_WRONLY) &&
           fifo_rendezvous("/tmp/fifo-wait", O_RDONLY) && interrupted_open();
  printf("[spore] FIFO open rendezvous and interruption: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}
