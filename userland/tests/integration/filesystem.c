#include "integration.h"

#include "dirent64.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define SYS_INOTIFY_INIT1 26
#define SYS_INOTIFY_ADD_WATCH 27
#define SYS_INOTIFY_RM_WATCH 28
#ifndef IN_NONBLOCK
#define IN_NONBLOCK 04000
#endif
#ifndef IN_CLOEXEC
#define IN_CLOEXEC 02000000
#endif

int integration_fs_demo(void) {
  int ok = 1;
  errno = 0;
  if (mkdir("/", 0777) == 0 || errno != EEXIST) { ok = 0; }
  if (mkdir("/tmp/spore-demo-d", 0777) != 0) { ok = 0; }
  int fd = open("/tmp/spore-demo-d/a", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd < 0) {
    ok = 0;
  } else {
    const char msg[] = "phase-d";
    const char iov_msg[] = "+iov";
    struct iovec iov[2] = {
      {.iov_base = (void *)0x1, .iov_len = 0},
      {.iov_base = (void *)iov_msg, .iov_len = sizeof(iov_msg) - 1},
    };
    if (write(fd, "", 0) != 0) { ok = 0; }
    if (write(fd, msg, sizeof(msg) - 1) != (ssize_t)(sizeof(msg) - 1)) { ok = 0; }
    if (writev(fd, iov, 2) != (ssize_t)(sizeof(iov_msg) - 1)) { ok = 0; }
    close(fd);
  }
  if (chdir("/tmp/spore-demo-d") != 0) { ok = 0; }
  char cwd[64];
  if (getcwd(cwd, sizeof(cwd)) == NULL || strcmp(cwd, "/tmp/spore-demo-d") != 0) { ok = 0; }
  fd = open("a", O_RDONLY);
  char buf[32] = {0};
  if (fd < 0 || read(fd, buf, sizeof(buf) - 1) != 11 || strcmp(buf, "phase-d+iov") != 0) { ok = 0; }
  if (fd >= 0) { close(fd); }
  if (rename("a", "b") != 0) { ok = 0; }
  int dfd = open(".", O_RDONLY);
  char dents[256];
  long dent_bytes = syscall(SYS_getdents64, dfd, dents, sizeof(dents));
  int saw_b = 0;
  for (long off = 0; off < dent_bytes;) {
    struct linux_dirent64 *d = (struct linux_dirent64 *)(dents + off);
    if (strcmp(d->d_name, "b") == 0) { saw_b = 1; }
    off += d->d_reclen;
  }
  if (dfd >= 0) { close(dfd); }
  if (!saw_b) { ok = 0; }
  int ifd = (int)syscall(SYS_INOTIFY_INIT1, IN_NONBLOCK | IN_CLOEXEC);
  if (ifd < 0) {
    ok = 0;
  } else {
    int wd = (int)syscall(SYS_INOTIFY_ADD_WATCH, ifd, "/tmp", 0xffffffffu);
    if (wd <= 0) { ok = 0; }
    errno = 0;
    char event_buf[32];
    if (read(ifd, event_buf, sizeof(event_buf)) != -1 || errno != EAGAIN) { ok = 0; }
    if (wd > 0 && syscall(SYS_INOTIFY_RM_WATCH, ifd, wd) != 0) { ok = 0; }
    close(ifd);
  }
  if (unlink("b") != 0) { ok = 0; }
  if (chdir("/") != 0) { ok = 0; }
  (void)syscall(35, AT_FDCWD, "/tmp/spore-demo-d", 0x200);
  printf("[spore] v2d fs/cwd demo: %s\n", ok ? "PASS" : "FAIL");
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
