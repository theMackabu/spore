#include "integration.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int namespace_init(pid_t outside, int ready, int inherited) {
  if (getpid() != 1 || getppid() != 0 || syscall(SYS_gettid) != 1) { return 1; }
  struct stat self, one;
  if (stat("/proc/self", &self) || stat("/proc/1", &one) || self.st_ino != one.st_ino) { return 2; }
  char foreign[64];
  snprintf(foreign, sizeof(foreign), "/proc/%d/status", outside);
  if (outside <= 1 || !kill(outside, 0) || errno != ESRCH || !access(foreign, F_OK) || errno != ENOENT) { return 3; }
  pid_t child = fork();
  if (!child) {
    if (getpid() != 2 || getppid() != 1 || unshare(CLONE_NEWPID) || getpid() != 2) { _exit(4); }
    pid_t nested = fork();
    if (!nested) { _exit(getpid() == 1 && getppid() == 0 ? 19 : 5); }
    int status;
    if (nested <= 2 || waitpid(nested, &status, 0) != nested || !WIFEXITED(status) || WEXITSTATUS(status) != 19) { _exit(6); }
    _exit(fork() == -1 && errno == ENOMEM ? 23 : 7);
  }
  int status;
  if (child != 2 || waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 23) { return 8; }
  int sync_pipe[2];
  if (pipe(sync_pipe)) { return 9; }
  child = fork();
  if (!child) {
    close(sync_pipe[0]);
    if (write(sync_pipe[1], "r", 1) != 1) { _exit(10); }
    for (;;) { poll(NULL, 0, -1); }
  }
  close(sync_pipe[1]);
  char c;
  if (child < 0 || read(sync_pipe[0], &c, 1) != 1 || write(ready, &c, 1) != 1) { return 11; }
  close(inherited);
  return 42;
}

static int check_pid_namespace(void) {
  int ready[2], lifetime[2];
  if (pipe(ready) || pipe(lifetime)) { return 0; }
  pid_t outside = getpid();
  pid_t init = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
  if (!init) {
    close(ready[0]);
    close(lifetime[0]);
    _exit(namespace_init(outside, ready[1], lifetime[1]));
  }
  close(ready[1]);
  close(lifetime[1]);
  char c;
  int status = -1;
  int ok = init > 1 && read(ready[0], &c, 1) == 1;
  if (init > 0) { ok &= waitpid(init, &status, 0) == init && WIFEXITED(status) && WEXITSTATUS(status) == 42; }
  struct pollfd p = {.fd = lifetime[0], .events = POLLIN};
  ok &= poll(&p, 1, 1000) == 1 && read(lifetime[0], &c, 1) == 0;
  close(ready[0]);
  close(lifetime[0]);
  printf("[spore] PID namespace init, nested wait and teardown: %s status=%d\n", ok ? "PASS" : "FAIL", status);
  return ok;
}


int integration_pid_namespace(void) {
  pid_t driver = fork();
  if (!driver) { _exit(check_pid_namespace() ? 0 : 1); }
  int status;
  return driver > 0 && waitpid(driver, &status, 0) == driver && !status;
}
