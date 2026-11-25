#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
  if (getpid() != 1) { return 1; }
  int fd = open("/marker", O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0 || write(fd, "snapshot\n", 9) != 9 || fsync(fd) || close(fd)) { return 2; }
  pid_t child = fork();
  if (child == 0) {
    execl("/reader", "reader", "/marker", (char *)0);
    _exit(3);
  }
  int status;
  if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status)) { return 4; }
  return WEXITSTATUS(status);
}
