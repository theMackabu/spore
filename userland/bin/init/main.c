#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static void print_motd(void) {
  int fd = open("/etc/motd", O_RDONLY);
  if (fd < 0) { return; }
  char buf[128];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) { break; }
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }
  close(fd);
}

int main(void) {
  char *const argv[] = {"/bin/msh", NULL};
  char *const envp[] = {
    "PATH=/bin:.",    "HOME=/home/spore",    "USER=spore",      "LOGNAME=spore",
    "SHELL=/bin/msh", "TERM=xterm-256color", "PWD=/home/spore", NULL,
  };
  print_motd();
  for (;;) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("init: fork");
      return 1;
    }
    if (pid == 0) {
      (void)chdir("/home/spore");
      if (setgid(1000) != 0 || setuid(1000) != 0) {
        perror("init: setuid spore");
        _exit(126);
      }
      execve("/bin/msh", argv, envp);
      perror("init: exec /bin/msh");
      _exit(127);
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
  }
}
