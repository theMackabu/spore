#include "integration.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SYS_MEMFD_CREATE 279

static int shared_mmap_regression(void) {
  int fd = open("/tmp/shared-mmap", O_CREAT | O_RDWR | O_TRUNC, 0666);
  int ok = fd >= 0;
  ok = ok && write(fd, "abcdef", 6) == 6;
  void *map = ok ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
  ok = ok && map != MAP_FAILED;
  char buf[8] = {0};
  if (ok) {
    char *p = map;
    p[1] = 'Z';
    p[2] = 'Y';
    lseek(fd, 0, SEEK_SET);
    ok = msync(map, 4096, MS_SYNC) == 0 && read(fd, buf, 6) == 6 && strcmp(buf, "aZYdef") == 0;
    ok = ok && munmap(map, 4096) == 0;
  }
  if (fd >= 0) {
    memset(buf, 0, sizeof(buf));
    lseek(fd, 0, SEEK_SET);
    ok = ok && read(fd, buf, 6) == 6;
    close(fd);
  }
  ok = ok && strcmp(buf, "aZYdef") == 0;
  printf("[spore] shared file mmap writeback: %s value=%s\n", ok ? "PASS" : "FAIL", buf);
  return ok;
}

static int shared_anon_mmap_fork_regression(void) {
  char *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  int ok = map != MAP_FAILED;
  if (ok) { map[0] = 'P'; }

  pid_t child = ok ? fork() : -1;
  if (child == 0) {
    int child_ok = map[0] == 'P';
    map[1] = child_ok ? 'C' : 'X';
    _exit(child_ok ? 0 : 1);
  }

  int status = 0;
  if (child > 0) { ok = ok && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0; }
  char first = map == MAP_FAILED ? '-' : map[0];
  char second = map == MAP_FAILED ? '-' : map[1];
  ok = ok && first == 'P' && second == 'C';
  if (map != MAP_FAILED) { ok = ok && munmap(map, 4096) == 0; }

  printf("[spore] shared anonymous mmap fork: %s value=%c%c\n", ok ? "PASS" : "FAIL", first, second);
  return ok;
}

static int shm_open_path_mmap_regression(void) {
  const char *path = "/dev/shm/spore-shm-test";
  unlink(path);
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
  int ok = fd >= 0 && ftruncate(fd, 4096) == 0;
  char *map = ok ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
  ok = ok && map != MAP_FAILED;
  char buf[8] = {0};
  if (ok) {
    memcpy(map, "shm", 3);
    ok = msync(map, 4096, MS_SYNC) == 0 && pread(fd, buf, 3, 0) == 3 && strcmp(buf, "shm") == 0;
    ok = ok && munmap(map, 4096) == 0;
  }
  if (fd >= 0) { close(fd); }
  unlink(path);
  printf("[spore] /dev/shm shared mmap: %s value=%s\n", ok ? "PASS" : "FAIL", buf);
  return ok;
}

static int count_dir_entries(const char *path) {
  DIR *dir = opendir(path);
  if (dir == NULL) { return -1; }
  int count = 0;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) { ++count; }
  }
  closedir(dir);
  return count;
}

static int memfd_shared_mmap_regression(void) {
  int before = count_dir_entries("/run/memfd");
  long fd_long = integration_raw_syscall3(SYS_MEMFD_CREATE, (long)"spore-memfd", MFD_CLOEXEC, 0);
  int fd = (int)fd_long;
  int ok = fd >= 0 && ftruncate(fd, 4096) == 0;
  char *map = ok ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
  ok = ok && map != MAP_FAILED;
  if (fd >= 0) {
    ok = ok && close(fd) == 0;
    fd = -1;
  }
  ok = ok && count_dir_entries("/run/memfd") == before;
  if (ok) {
    map[0] = 'M';
    pid_t child = fork();
    if (child == 0) {
      int child_ok = map[0] == 'M';
      map[1] = child_ok ? 'C' : 'X';
      _exit(child_ok ? 0 : 1);
    }
    int status = 0;
    ok = child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
         map[0] == 'M' && map[1] == 'C';
    ok = ok && munmap(map, 4096) == 0;
  }
  if (fd >= 0) { close(fd); }
  ok = ok && count_dir_entries("/run/memfd") == before;
  printf("[spore] memfd shared mmap fork: %s fd=%ld visible-before=%d\n", ok ? "PASS" : "FAIL", fd_long, before);
  return ok;
}

static int memfd_seals_regression(void) {
  int no_seal_fd = (int)integration_raw_syscall3(SYS_MEMFD_CREATE, (long)"spore-no-seal", MFD_CLOEXEC, 0);
  errno = 0;
  int no_seal_get = no_seal_fd >= 0 ? fcntl(no_seal_fd, F_GET_SEALS) : -1;
  int no_seal_errno = errno;
  errno = 0;
  int no_seal_add = no_seal_fd >= 0 ? fcntl(no_seal_fd, F_ADD_SEALS, F_SEAL_GROW) : -1;
  int no_seal_add_errno = errno;
  int ok = no_seal_fd >= 0 && no_seal_get == F_SEAL_SEAL && no_seal_errno == 0 && no_seal_add == -1 &&
           no_seal_add_errno == EPERM;
  if (no_seal_fd >= 0) { close(no_seal_fd); }

  int fd = (int)integration_raw_syscall3(SYS_MEMFD_CREATE, (long)"spore-seals", MFD_CLOEXEC | MFD_ALLOW_SEALING, 0);
  ok = ok && fd >= 0 && ftruncate(fd, 4096) == 0 && write(fd, "abc", 3) == 3;
  ok = ok && fcntl(fd, F_GET_SEALS) == 0;
  ok = ok && fcntl(fd, F_ADD_SEALS, F_SEAL_GROW) == 0;
  errno = 0;
  ok = ok && ftruncate(fd, 8192) == -1 && errno == EPERM;
  ok = ok && ftruncate(fd, 2048) == 0;
  ok = ok && fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0;
  errno = 0;
  ok = ok && ftruncate(fd, 1024) == -1 && errno == EPERM;
  ok = ok && fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0;
  errno = 0;
  ok = ok && write(fd, "z", 1) == -1 && errno == EPERM;
  errno = 0;
  void *future = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ok = ok && future == MAP_FAILED && errno == EPERM;
  int seals = fcntl(fd, F_GET_SEALS);
  ok = ok && (seals & (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE)) ==
               (F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE);
  ok = ok && fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL) == 0;
  errno = 0;
  ok = ok && fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EPERM;
  if (fd >= 0) { close(fd); }

  int busy_fd = (int)integration_raw_syscall3(SYS_MEMFD_CREATE, (long)"spore-seal-busy", MFD_ALLOW_SEALING, 0);
  ok = ok && busy_fd >= 0 && ftruncate(busy_fd, 4096) == 0;
  char *map = ok ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, busy_fd, 0) : MAP_FAILED;
  ok = ok && map != MAP_FAILED;
  errno = 0;
  ok = ok && fcntl(busy_fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY;
  if (map != MAP_FAILED) { ok = ok && munmap(map, 4096) == 0; }
  ok = ok && fcntl(busy_fd, F_ADD_SEALS, F_SEAL_WRITE) == 0;
  errno = 0;
  ok = ok && write(busy_fd, "x", 1) == -1 && errno == EPERM;
  if (busy_fd >= 0) { close(busy_fd); }

  printf("[spore] memfd seals: %s seals=0x%x\n", ok ? "PASS" : "FAIL", seals);
  return ok;
}

static int growdown_mmap_regression(void) {
#ifndef MAP_GROWSDOWN
#define MAP_GROWSDOWN 0x0100
#endif
  char *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN, -1, 0);
  int ok = map != MAP_FAILED;
  if (ok) {
    char *low = map - 3 * 4096;
    low[0] = 'g';
    ok = low[0] == 'g' && munmap(low, 4 * 4096) == 0;
  }
  printf("[spore] growdown mmap multi-page fault: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int mmap_fixed_noreplace_regression(void) {
  char *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int ok = map != MAP_FAILED;
  errno = 0;
  void *again =
    ok ? mmap(map, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) : MAP_FAILED;
  ok = ok && again == MAP_FAILED && errno == EEXIST;
  if (map != MAP_FAILED) { ok = ok && munmap(map, 4096) == 0; }
  printf("[spore] mmap fixed-noreplace collision: %s errno=%d\n", ok ? "PASS" : "FAIL", errno);
  return ok;
}

static int mmap_noreserve_regression(void) {
  char *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  int ok = map != MAP_FAILED;
  if (ok) {
    map[0] = 'N';
    ok = map[0] == 'N' && munmap(map, 4096) == 0;
  }
  printf("[spore] mmap noreserve anonymous: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

int integration_memory_suite(void) {
  return shared_mmap_regression() && shared_anon_mmap_fork_regression() && shm_open_path_mmap_regression() &&
         memfd_shared_mmap_regression() && memfd_seals_regression() && growdown_mmap_regression() &&
         mmap_fixed_noreplace_regression() && mmap_noreserve_regression();
}
