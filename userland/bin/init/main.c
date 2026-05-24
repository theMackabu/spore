#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

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
  print_motd();
  for (;;) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("init: fork");
      return 1;
    }
    if (pid == 0) {
      execve("/bin/msh", argv, environ);
      perror("init: exec /bin/msh");
      _exit(127);
    }
    int status = 0;
    (void)waitpid(pid, &status, 0);
  }
}
