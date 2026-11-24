#include "integration.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

constexpr int SYS_MEMFD_CREATE = 279;

static int sparse_shared_file(const char *path) {
  constexpr size_t length = 8 * 1024 * 1024;
  int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (fd < 0) { return 0; }
  int ok = ftruncate(fd, length) == 0;
  volatile char *map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  struct stat st;
  if (map != MAP_FAILED) {
    ok = ok && map[4096] == 0 && map[length - 4096] == 0;
    ok = ok && msync((void *)map, length, MS_SYNC) == 0 && fstat(fd, &st) == 0 && st.st_blocks == 0;
    map[length - 1] = 'Z';
    ok = ok && msync((void *)map, length, MS_SYNC) == 0 && fstat(fd, &st) == 0 &&
         st.st_size == length && st.st_blocks > 0 && st.st_blocks < length / 512;
    munmap((void *)map, length);
    ok = ok && ftruncate(fd, 0) == 0 && fstat(fd, &st) == 0 && st.st_blocks == 0;
  } else { ok = 0; }
  close(fd);
  unlink(path);
  printf("[spore] sparse shared file %s: %s\n", path, ok ? "PASS" : "FAIL");
  return ok;
}

static int sync_file_aliases(void) {
  constexpr size_t page = 4096;
  int fd = open("/mapped-aliases", O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) { return 0; }
  int ok = ftruncate(fd, 3 * page) == 0;
  char *writer = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  char *other = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  char *reader = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, page);
  int ready[2], resume[2];
  if (ok && writer != MAP_FAILED && other != MAP_FAILED && reader != MAP_FAILED &&
      pipe(ready) == 0 && pipe(resume) == 0) {
    writer[0] = 'P';
    other[1] = 'Q';
    pid_t child = fork();
    if (child == 0) {
      close(ready[0]);
      close(resume[1]);
      writer[page] = 'C';
      char byte;
      _exit(write(ready[1], "!", 1) == 1 && read(resume[0], &byte, 1) == 1 ? 0 : 1);
    }
    close(ready[1]);
    close(resume[0]);
    char byte;
    struct stat partial, full;
    ok = child > 0 && read(ready[0], &byte, 1) == 1 && msync(reader, page, MS_SYNC) == 0 &&
         fstat(fd, &partial) == 0 && partial.st_blocks > 0 && reader[0] == 'C';
    if (child > 0) {
      int status;
      ok = (write(resume[1], "!", 1) == 1) && ok;
      ok = (waitpid(child, &status, 0) == child && status == 0) && ok;
    }
    close(ready[0]);
    close(resume[1]);
    ok = ok && msync(writer, page, MS_SYNC) == 0 && fstat(fd, &full) == 0 && full.st_blocks > partial.st_blocks;
    ok = ok && pread(fd, writer + 2 * page, 1, page) == 1 && fsync(fd) == 0 &&
         fstat(fd, &partial) == 0 && partial.st_blocks > full.st_blocks;
    other[1] = 'R';
    writer[0] = 'S';
    ok = ok && msync(other, page, MS_SYNC) == 0;
  } else { ok = 0; }
  if (writer != MAP_FAILED) { munmap(writer, 3 * page); }
  if (other != MAP_FAILED) { munmap(other, 3 * page); }
  if (reader != MAP_FAILED) { munmap(reader, page); }
  char data[2];
  ok = ok && pread(fd, data, 2, 0) == 2 && memcmp(data, "SR", 2) == 0 &&
       pread(fd, data, 1, 2 * page) == 1 && data[0] == 'C';
  close(fd);
  unlink("/mapped-aliases");
  printf("[spore] shared writeback across aliases and fork: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

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
    struct stat st;
    ok = read(fd, buf, 6) == 6 && strcmp(buf, "aZYdef") == 0;
    ok = ok && pwrite(fd, "J", 1, 1) == 1 && p[1] == 'J';
    ok = ok && msync(map, 4096, MS_SYNC) == 0 && fstat(fd, &st) == 0 && st.st_size == 6;
    ok = ok && ftruncate(fd, 3) == 0 && p[3] == 0 && msync(map, 4096, MS_SYNC) == 0;
    ok = ok && munmap(map, 4096) == 0;
  }
  if (fd >= 0) {
    memset(buf, 0, sizeof(buf));
    lseek(fd, 0, SEEK_SET);
    ok = ok && read(fd, buf, 6) == 3;
    close(fd);
  }
  ok = ok && strcmp(buf, "aJY") == 0;
  printf("[spore] shared file mmap writeback: %s value=%s\n", ok ? "PASS" : "FAIL", buf);
  return ok;
}

static int shared_anon_mmap_fork_regression(void) {
  char *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  int ok = map != MAP_FAILED;
  if (ok) { map[0] = 'P'; }

  pid_t child = ok ? fork() : -1;
  if (child == 0) {
    int child_ok = madvise(map, 4096, MADV_DONTNEED) == 0 && map[0] == 'P';
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
  if (map != MAP_FAILED) {
    ok = ok && fcntl(busy_fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) == 0;
    map[0] = 'W';
    ok = ok && msync(map, 4096, MS_SYNC) == 0 && mprotect(map, 4096, PROT_READ) == 0;
    ok = ok && fcntl(busy_fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY;
    ok = ok && mprotect(map, 4096, PROT_READ | PROT_WRITE) == 0;
    if (ok) { map[1] = 'B'; }
    ok = ok && munmap(map, 4096) == 0;
    char saved[2];
    ok = ok && pread(busy_fd, saved, sizeof(saved), 0) == 2 && memcmp(saved, "WB", 2) == 0;
  }
  ok = ok && fcntl(busy_fd, F_ADD_SEALS, F_SEAL_WRITE) == 0;
  errno = 0;
  ok = ok && write(busy_fd, "x", 1) == -1 && errno == EPERM;
  if (busy_fd >= 0) { close(busy_fd); }

  printf("[spore] memfd seals: %s seals=0x%x\n", ok ? "PASS" : "FAIL", seals);
  return ok;
}

static int growdown_mmap_regression(void) {
#ifndef MAP_GROWSDOWN
  constexpr int MAP_GROWSDOWN = 0x0100;
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

static int file_mapping_access(void) {
  size_t page = 4096;
  const char *path = "/tmp/mmap-access";
  int writer = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  int reader = open(path, O_RDONLY);
  int ok = writer >= 0 && reader >= 0 && write(writer, "F", 1) == 1;
  char byte;
  ok = ok && pread(writer, &byte, 1, 0) == -1 && errno == EBADF &&
       pwrite(reader, "X", 1, 0) == -1 && errno == EBADF &&
       read(writer, &byte, 1) == -1 && errno == EBADF && write(reader, "X", 1) == -1 && errno == EBADF &&
       ftruncate(reader, 0) == -1 && errno == EINVAL;
  char *original = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (original != MAP_FAILED) {
    original[0] = 'A';
    void *replacement = mmap(original, page, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, reader, 0);
    ok = ok && replacement == MAP_FAILED && errno == EACCES && original[0] == 'A';
    replacement = mmap(original, page, PROT_READ, MAP_FIXED | MAP_PRIVATE, writer, 0);
    ok = ok && replacement == MAP_FAILED && errno == EACCES && original[0] == 'A';
    char *shared = mmap(NULL, page, PROT_READ, MAP_SHARED, reader, 0);
    if (shared != MAP_FAILED) {
      ok = ok && mprotect(shared, page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES && shared[0] == 'F';
      munmap(shared, page);
    } else { ok = 0; }
    char *copy = mmap(original, page, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, reader, 0);
    if (copy == original) {
      ok = ok && copy[0] == 'F';
      copy[0] = 'C';
      char data;
      ok = ok && pread(reader, &data, 1, 0) == 1 && data == 'F';
    } else { ok = 0; }
    munmap(original, page);
  } else { ok = 0; }
  if (reader >= 0) { close(reader); }
  if (writer >= 0) { close(writer); }
  unlink(path);
  printf("[spore] file mapping access: %s\n", ok ? "PASS" : "FAIL");
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

static int mapping_ranges(void) {
  constexpr size_t page = 4096;
  char *p = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { return 0; }
  p[0] = 'A';
  int ok = munmap(p + 1, page) == -1 && errno == EINVAL &&
           syscall(SYS_mprotect, p + 1, page, PROT_READ) == -1 && errno == EINVAL &&
           madvise(p + 1, page, MADV_DONTNEED) == -1 && errno == EINVAL && p[0] == 'A';
  ok = ok && munmap(p + page, page) == 0 && mprotect(p, 3 * page, PROT_READ) == -1 && errno == ENOMEM &&
       msync(p, 3 * page, MS_SYNC) == -1 && errno == ENOMEM;
  ok = ok && madvise(p, page, MADV_DONTNEED) == 0 && p[0] == 0;
  munmap(p, 3 * page);
  printf("[spore] mapping ranges and discard: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int grow_file_mapping(void) {
  constexpr size_t page = 4096;
  int fd = open("/tmp/remap-file", O_CREAT | O_RDWR | O_TRUNC, 0600);
  if (fd < 0) { return 0; }
  int ok = pwrite(fd, "B", 1, 2 * page) == 1 && pwrite(fd, "C", 1, 3 * page) == 1;
  char *base = mmap(NULL, 3 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base != MAP_FAILED) {
    munmap(base, 3 * page);
    char *file = mmap(base, page, PROT_READ, MAP_PRIVATE | MAP_FIXED_NOREPLACE, fd, page);
    if (file == base) {
      ok = ok && mremap(base, page, 2 * page, 0) == base && base[page] == 'B';
      ok = ok && mremap(base, 2 * page, 3 * page, 0) == base && base[2 * page] == 'C';
      munmap(base, 3 * page);
    } else { ok = 0; }
  } else { ok = 0; }
  close(fd);
  unlink("/tmp/remap-file");
  printf("[spore] repeated file mapping growth: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int mmap_past_reservation(bool move) {
  constexpr size_t page = 4096;
  constexpr size_t reserve_size = 64 * 1024 * 1024;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  char *original = mmap(NULL, page, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (original == MAP_FAILED) { return 0; }
  original[0] = 'a';
  void *reservation = mmap(original + page, reserve_size, PROT_NONE, flags | MAP_FIXED_NOREPLACE, -1, 0);
  int ok = 0;
  if (reservation != MAP_FAILED) {
    size_t len = move ? 2 * page : page;
    char *result = move ? mremap(original, page, len, MREMAP_MAYMOVE)
                        : mmap(NULL, len, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (result != MAP_FAILED) {
      ok = !move || result[0] == 'a';
      result[len - 1] = 'z';
      ok = ok && result[len - 1] == 'z';
      munmap(result, len);
      if (move) { original = MAP_FAILED; }
    }
    munmap(reservation, reserve_size);
  }
  if (original != MAP_FAILED) { munmap(original, page); }
  printf("[spore] %s past 64 MiB reservation: %s\n", move ? "mremap" : "mmap", ok ? "PASS" : "FAIL");
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
  return sync_file_aliases() && sparse_shared_file("/spore-sparse") && sparse_shared_file("/tmp/spore-sparse") && shared_mmap_regression() && shared_anon_mmap_fork_regression() && shm_open_path_mmap_regression() &&
         memfd_shared_mmap_regression() && memfd_seals_regression() && growdown_mmap_regression() &&
         file_mapping_access() && mmap_fixed_noreplace_regression() && mmap_noreserve_regression() && mapping_ranges() && grow_file_mapping() && mmap_past_reservation(false) &&
         mmap_past_reservation(true);
}
