#include <fcntl.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  if (getpid() != 1) { return 1; }
  if (!access("/reboot-test", F_OK)) {
    if (!access("/marker", F_OK)) {
      char header[4];
      int boot = open("/dev/blk/boot", O_RDONLY);
      return boot >= 0 && read(boot, header, 4) == 4 && !memcmp(header, "ESP!", 4) ? 42 : 6;
    }
    int out = open("/marker", O_CREAT | O_WRONLY, 0600);
    if (out < 0 || close(out)) { return 7; }
    syscall(0x4006, 1);
    return 8;
  }
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
