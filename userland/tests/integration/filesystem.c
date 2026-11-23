#include "integration.h"

#include "dirent64.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int inotify_shared_watch(void) {
  int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd < 0) { return 0; }
  int wd = inotify_add_watch(fd, "/tmp", IN_MODIFY);
  int alias = dup(fd);
  close(fd);
  if (alias < 0) { return 0; }
  char events[32];
  int ok = wd > 0 && read(alias, events, sizeof(events)) == -1 && errno == EAGAIN;
  pid_t child = fork();
  if (child == 0) { _exit(inotify_rm_watch(alias, wd) == 0 ? 0 : 1); }
  int status;
  ok = child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && ok;
  ok = ok && inotify_rm_watch(alias, wd) == -1 && errno == EINVAL &&
       inotify_add_watch(alias, "/tmp", IN_MODIFY) > 0;
  close(alias);
  return ok;
}

static int epoll_watch_lifetime(void) {
  int counter = eventfd(1, EFD_NONBLOCK);
  if (counter < 0) { return 0; }
  struct epoll_event watch = {.events = EPOLLIN, .data.fd = counter};
  struct epoll_event event;
  int ep = epoll_create1(0);
  int ok = ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, counter, &watch) == 0 &&
           epoll_wait(ep, &event, 1, 0) == 1 && (event.events & EPOLLIN) && event.data.fd == counter;
  if (ep >= 0) { close(ep); }

  eventfd_t value;
  int next = eventfd(2, EFD_NONBLOCK);
  ok = ok && next >= 0 && eventfd_read(next, &value) == 0 && value == 2;
  if (next >= 0) { close(next); }
  ep = epoll_create1(0);
  ok = ok && ep >= 0 && epoll_wait(ep, &event, 1, 0) == 0;
  if (ep >= 0) { close(ep); }
  ok = ok && eventfd_read(counter, &value) == 0 && value == 1;
  close(counter);
  return ok;
}

static int fifo_without_reader(void) {
  for (int i = 0; i < 32; ++i) {
    char path[32];
    snprintf(path, sizeof(path), "fifo-%d", i);
    if (mkfifo(path, 0600) != 0) { return 0; }
    int writer = open(path, O_WRONLY | O_NONBLOCK);
    if (writer >= 0) {
      close(writer);
      return 0;
    }
    if (errno != ENXIO) { return 0; }
  }
  int reader = open("fifo-0", O_RDONLY | O_NONBLOCK);
  int writer = open("fifo-0", O_WRONLY | O_NONBLOCK);
  char byte;
  int ok = reader >= 0 && writer >= 0 && write(writer, "x", 1) == 1 && read(reader, &byte, 1) == 1 && byte == 'x';
  if (writer >= 0) { close(writer); }
  if (reader >= 0) { close(reader); }
  for (int i = 0; i < 32; ++i) {
    char path[32];
    snprintf(path, sizeof(path), "fifo-%d", i);
    unlink(path);
  }
  return ok;
}

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
  if (!inotify_shared_watch() || !epoll_watch_lifetime() || !fifo_without_reader()) { ok = 0; }
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
