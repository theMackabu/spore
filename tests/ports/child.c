#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void) {
  volatile int *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  volatile int *private = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (shared == MAP_FAILED || private == MAP_FAILED) { return 1; }
  *private = 3;
  pid_t pid = fork();
  if (pid == 0) {
    struct timespec delay = {.tv_nsec = 20000000};
    if (nanosleep(&delay, NULL)) { _exit(1); }
    *private = 9;
    *shared = 7;
    _exit(42);
  }
  int status;
  if (pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42 && *private == 3 &&
      *shared == 7) {
    write(1, "child wait passed\n", 18);
  }
  syscall(0x4006, 0);
  return 1;
}
