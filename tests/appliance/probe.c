#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int ready;

static void *worker(void *arg) {
  pthread_mutex_lock(&lock);
  ready = 1;
  pthread_cond_signal(&changed);
  pthread_mutex_unlock(&lock);
  return arg;
}

#ifdef WITH_LIBRARY
extern int fixture(void);
#endif

int main(int argc, char **argv) {
#ifdef WITH_LIBRARY
  if (fixture() != 42) { return 8; }
#endif
  if (argc == 2 && !strcmp(argv[1], "orphan")) {
    int fd = open("/data/scratch", O_CREAT | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, 4096) || unlink("/data/scratch")) { return 1; }
    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { return 1; }
    p[0] = 'x';
    return 0;
  }
  if (argc == 2 && !strcmp(argv[1], "signal")) {
    raise(SIGTERM);
    return 1;
  }
  char path[256] = {0}, cwd[256];
  if (argc != 3 || strcmp(argv[0], "custom name") || strcmp(argv[1], "two words") || strcmp(argv[2], "") ||
      strcmp((const char *)getauxval(AT_EXECFN), "/app/server") || !getenv("CHECK") || strcmp(getenv("CHECK"), "a=b") ||
      !getcwd(cwd, sizeof(cwd)) || strcmp(cwd, "/work") || readlink("/proc/self/exe", path, sizeof(path) - 1) < 0 ||
      strcmp(path, "/app/server")) {
    return 2;
  }
  if (fork() != -1 || errno != EPERM || syscall(SYS_clone, 0x4100 | SIGCHLD, 0, 0, 0, 0) != -1 || errno != EPERM ||
      syscall(SYS_clone, 0x100 | SIGCHLD, 0, 0, 0, 0) != -1 || errno != EPERM || syscall(0x4001, 0, 0, 0) != -1 ||
      errno != EPERM) {
    return 3;
  }
  pthread_t t;
  pthread_mutex_lock(&lock);
  if (pthread_create(&t, NULL, worker, &ready)) { return 4; }
  while (!ready) {
    pthread_cond_wait(&changed, &lock);
  }
  pthread_mutex_unlock(&lock);
  void *result;
  if (pthread_join(t, &result) || result != &ready) { return 5; }
  int f = open("/tmp/test", O_RDWR | O_CREAT, 0600);
  if (f < 0 || write(f, "x", 1) != 1 || close(f) || unlink("/tmp/test")) { return 6; }
  if (open("/app/server", O_WRONLY) >= 0 || errno != EROFS || open("/unexpected", O_CREAT | O_RDWR, 0600) >= 0 ||
      errno != EROFS || access("/etc/passwd", F_OK) == 0 || access("/bin/sh", F_OK) == 0) {
    return 7;
  }
  puts("app contract passed");
  return 42;
}
