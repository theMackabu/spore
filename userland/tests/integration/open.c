#include "integration.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int deep_directory(void) {
  char path[128] = "/deep-dir-check", cwd[128];
  size_t base = strlen(path), len = base;
  if (mkdir(path, 0700)) { return 0; }
  int ok = 1;
  for (int i = 0; ok && i < 24; ++i) {
    memcpy(path + len, "/d", 3);
    if (mkdir(path, 0700)) { path[len] = 0; ok = 0; break; }
    len += 2;
  }
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  struct stat st;
  ok = ok && fd >= 0 && fstatat(fd, "", &st, AT_EMPTY_PATH) == 0 && S_ISDIR(st.st_mode) &&
       fstatat(fd, "", &st, 0) == -1 && errno == ENOENT;
  if (fd >= 0) { close(fd); }
  if (ok) {
    pid_t child = fork();
    if (!child) {
      _exit(chdir(path) == 0 && syscall(SYS_getcwd, cwd, len) == -1 && errno == ERANGE &&
            syscall(SYS_getcwd, cwd, sizeof(cwd)) == (long)len + 1 && !strcmp(cwd, path) &&
            chdir("../.././d") == 0 && syscall(SYS_getcwd, cwd, sizeof(cwd)) == (long)len - 1 ? 0 : 1);
    }
    int status;
    ok = child > 0 && waitpid(child, &status, 0) == child && status == 0;
  }
  while (len > base) {
    if (rmdir(path)) { ok = 0; }
    len -= 2; path[len] = 0;
  }
  return rmdir(path) == 0 && ok;
}

static int creation_mask(void) {
  mode_t saved = umask(0077);
  pid_t child = fork();
  if (child == 0) {
    struct stat st;
    int ok = umask(0027) == 0077;
    int fd = open("/tmp/masked", O_CREAT | O_EXCL | O_RDWR, 0666);
    ok = ok && fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0640;
    if (fd >= 0) { close(fd); unlink("/tmp/masked"); }
    _exit(ok ? 0 : 1);
  }
  int status;
  int ok = child > 0 && waitpid(child, &status, 0) == child && status == 0;
  struct stat st;
  int fd = open("/", O_TMPFILE | O_RDWR, 0777);
  ok = ok && fd >= 0 && fstat(fd, &st) == 0 && (st.st_mode & 0777) == 0700;
  if (fd >= 0) { close(fd); }
  return umask(saved) == 0077 && ok;
}

static int open_flags(void) {
  const char *name = "/open-flags", *alias = "/open-alias";
  struct stat st;
  char text[32], proc[48];
  int fd = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) { return 0; }
  int ok = write(fd, "abc", 3) == 3 && symlink(name, alias) == 0 &&
           open(alias, O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP &&
           open(name, O_DIRECTORY) == -1 && errno == ENOTDIR &&
           open("/open-flags/", O_RDONLY) == -1 && errno == ENOTDIR &&
           open("/", O_WRONLY) == -1 && errno == EISDIR;
  int path = open(alias, O_PATH | O_NOFOLLOW | O_RDWR | O_TRUNC | O_CLOEXEC);
  ok = ok && path >= 0 && fstat(path, &st) == 0 && S_ISLNK(st.st_mode) &&
       fcntl(path, F_GETFL) == (O_PATH | O_NOFOLLOW) && (fcntl(path, F_GETFD) & FD_CLOEXEC) &&
       read(path, text, 1) == -1 && errno == EBADF && unlink(alias) == 0 &&
       readlinkat(path, "", text, sizeof(text)) == (ssize_t)strlen(name) && !memcmp(name, text, strlen(name));
  if (path >= 0) { close(path); }
  snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
  int reopened = open(proc, O_RDONLY);
  ok = ok && reopened >= 0 && read(reopened, text, 3) == 3 && !memcmp(text, "abc", 3) &&
       write(reopened, "!", 1) == -1 && errno == EBADF && lseek(fd, 0, SEEK_CUR) == 3;
  if (reopened >= 0) { close(reopened); }
  close(fd); unlink(alias); unlink(name);
  return ok;
}

static int inode_times(const char *name) {
  int fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) { return 0; }
  struct stat st;
  struct timespec ts[2] = {{.tv_sec = -12}, {.tv_sec = 0}};
  int ok = futimens(fd, ts) == 0 && stat(name, &st) == 0 && st.st_atim.tv_sec == -12 && st.st_mtim.tv_sec == 0;
  ts[0].tv_nsec = UTIME_OMIT;
  ts[1].tv_sec = 98765;
  ok = ok && utimensat(AT_FDCWD, name, ts, 0) == 0 && fstat(fd, &st) == 0 &&
       st.st_mtim.tv_sec == 98765 && st.st_atim.tv_sec == -12;
  ts[1].tv_nsec = 1000000000;
  ok = ok && futimens(fd, ts) == -1 && errno == EINVAL;
  ts[1].tv_nsec = UTIME_NOW;
  ok = ok && unlink(name) == 0 && futimens(fd, ts) == 0 && fstat(fd, &st) == 0 &&
       st.st_nlink == 0 && st.st_atim.tv_sec == -12 && st.st_mtim.tv_sec > 98765;
  close(fd);
  unlink(name);
  return ok;
}

int integration_open_suite(void) {
  int ok = creation_mask() && deep_directory() && open_flags() && inode_times("/inode-times") && inode_times("/tmp/inode-times");
  printf("[spore] pathname, umask and descriptor opens: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

int integration_tmpfile(const char *directory) {
  int dir = open(directory, O_RDONLY | O_DIRECTORY);
  int fd = -1, named = -1, ok = 0;
  struct stat st, published;
  char procfd[64], data[4];
  const char *name = "spore-tmpfile-check";
  if (dir < 0) { goto done; }
  fd = openat(dir, ".", O_TMPFILE | O_RDONLY, 0600);
  if (fd >= 0 || errno != EINVAL) { goto done; }
  fd = openat(dir, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0640);
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0777) != 0640 || st.st_nlink ||
      !(fcntl(fd, F_GETFD) & FD_CLOEXEC) || write(fd, "data", 4) != 4) {
    goto done;
  }
  snprintf(procfd, sizeof(procfd), "/proc/self/fd/%d", fd);
  if (linkat(AT_FDCWD, procfd, dir, name, AT_SYMLINK_FOLLOW) ||
      linkat(AT_FDCWD, procfd, dir, name, AT_SYMLINK_FOLLOW) == 0 || errno != EEXIST) {
    goto done;
  }
  named = openat(dir, name, O_RDWR);
  if (named < 0 || fstat(named, &published) || published.st_ino != st.st_ino || published.st_dev != st.st_dev ||
      published.st_nlink != 1 || read(named, data, 4) != 4 || memcmp(data, "data", 4)) {
    goto done;
  }
  if (linkat(AT_FDCWD, procfd, AT_FDCWD, !strcmp(directory, "/") ? "/tmp/spore-cross-link" : "/spore-cross-link",
             AT_SYMLINK_FOLLOW) == 0 ||
      errno != EXDEV) {
    goto done;
  }
  if (geteuid() == 0) {
    if (linkat(fd, "", dir, "spore-tmpfile-alias", AT_EMPTY_PATH) || fstat(named, &published) ||
        published.st_nlink != 2 || unlinkat(dir, "spore-tmpfile-alias", 0)) {
      goto done;
    }
  }
  if (unlinkat(dir, name, 0) || fstat(named, &published) || published.st_nlink || lseek(named, 0, SEEK_SET) ||
      read(named, data, 4) != 4 || memcmp(data, "data", 4)) {
    goto done;
  }

  if (linkat(AT_FDCWD, procfd, dir, name, AT_SYMLINK_FOLLOW) == 0 || errno != ENOENT) { goto done; }
  void *mapping = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapping == MAP_FAILED) { goto done; }
  close(fd);
  close(named);
  fd = named = -1;
  int readable = memcmp(mapping, "data", 4) == 0;
  munmap(mapping, 4096);
  if (!readable) { goto done; }

  fd = openat(dir, ".", O_WRONLY | O_TMPFILE | O_EXCL, 0600);
  if (fd < 0) { goto done; }
  snprintf(procfd, sizeof(procfd), "/proc/self/fd/%d", fd);
  ok = linkat(AT_FDCWD, procfd, dir, name, AT_SYMLINK_FOLLOW) == -1 && errno == ENOENT;
done:
  printf("[spore] O_TMPFILE %s ... %s\n", directory, ok ? "ok" : "FAIL");
  if (fd >= 0) { close(fd); }
  if (named >= 0) { close(named); }
  if (dir >= 0) {
    unlinkat(dir, "spore-tmpfile-alias", 0);
    unlinkat(dir, name, 0);
    close(dir);
  }
  return ok;
}
