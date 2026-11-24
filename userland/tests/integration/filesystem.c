#include "integration.h"

#include "dirent64.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <sys/time.h>
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

static int getdents_proc_batches(void) {
  int fd = open("/proc/self", O_RDONLY | O_DIRECTORY);
  if (fd < 0) { return 0; }
  unsigned seen = 0;
  char dents[32];
  for (;;) {
    long bytes = syscall(SYS_getdents64, fd, dents, sizeof(dents));
    if (bytes < 0) {
      close(fd);
      return 0;
    }
    if (bytes == 0) { break; }
    for (long off = 0; off < bytes;) {
      struct linux_dirent64 *ent = (struct linux_dirent64 *)(dents + off);
      if (ent->d_reclen == 0 || off + ent->d_reclen > bytes) {
        close(fd);
        return 0;
      }
      unsigned bit = 0;
      if (strcmp(ent->d_name, "stat") == 0) {
        bit = 1;
      } else if (strcmp(ent->d_name, "cwd") == 0) {
        if (ent->d_type != DT_LNK) {
          close(fd);
          return 0;
        }
        bit = 2;
      } else if (strcmp(ent->d_name, "exe") == 0) {
        if (ent->d_type != DT_LNK) {
          close(fd);
          return 0;
        }
        bit = 4;
      }
      if (bit != 0 && (seen & bit) != 0) {
        close(fd);
        return 0;
      }
      seen |= bit;
      off += ent->d_reclen;
    }
  }
  close(fd);
  return seen == 7;
}

static int getdents_fault_resume(void) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char *buf = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (buf == MAP_FAILED) { return 0; }
  int reference = open("/proc/self", O_RDONLY | O_DIRECTORY);
  int fd = open("/proc/self", O_RDONLY | O_DIRECTORY);
  char expected[512], resumed[512];
  long size = reference < 0 ? -1 : syscall(SYS_getdents64, reference, expected, sizeof(expected));
  int ok = size > 0 && fd >= 0 && munmap(buf + page, page) == 0;
  if (ok) {
    struct linux_dirent64 *first = (struct linux_dirent64 *)expected;
    size_t len = first->d_reclen;
    char *split = buf + page - len;
    ok = len < (size_t)size && syscall(SYS_getdents64, fd, split, sizeof(expected)) == (long)len &&
         strcmp(((struct linux_dirent64 *)split)->d_name, first->d_name) == 0;
    ok = ok && syscall(SYS_getdents64, fd, buf + page - 1, sizeof(resumed)) == -1 && errno == EFAULT;
    long bytes = syscall(SYS_getdents64, fd, resumed, sizeof(resumed));
    ok = ok && bytes == size - (long)len &&
         strcmp(((struct linux_dirent64 *)resumed)->d_name, ((struct linux_dirent64 *)(expected + len))->d_name) == 0;
  }
  if (fd >= 0) { close(fd); }
  if (reference >= 0) { close(reference); }
  munmap(buf, page * 2);
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

static int pathname_page_boundary(void) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  char *buf = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (buf == MAP_FAILED) { return 0; }
  const char name[] = "/etc/motd";
  struct stat st;
  char *split = buf + page - 3;
  memcpy(split, name, sizeof(name));
  int ok = stat(split, &st) == 0;
  if (munmap(buf + page, page) != 0) { ok = 0; }
  char *last = buf + page - sizeof(name);
  memcpy(last, name, sizeof(name));
  ok = ok && stat(last, &st) == 0;
  buf[page - 1] = 'x';
  ok = ok && stat(last, &st) == -1 && errno == EFAULT;
  munmap(buf, page);
  return ok;
}

static void flock_alarm(int signal) {
  (void)signal;
}

static int flock_regression(void) {
  const char *path = "/flock-test";
  const char *link_path = "/flock-test-link";
  int a = open(path, O_CREAT | O_RDWR | O_EXCL, 0600);
  if (a < 0) { return 0; }
  int b = link(path, link_path) == 0 ? open(link_path, O_RDWR) : -1;
  unlink(link_path);
  unlink(path);
  int ready[2];
  if (b < 0 || pipe(ready) < 0) {
    close(a);
    if (b >= 0) { close(b); }
    return 0;
  }

  int alias = dup(a);
  int ok = alias >= 0 && flock(a, LOCK_SH) == 0 && flock(b, LOCK_SH) == 0 &&
           flock(alias, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK &&
           flock(b, LOCK_EX | LOCK_NB) == 0 && flock(b, LOCK_UN) == 0 && flock(alias, LOCK_EX) == 0;
  close(a);
  pid_t child = ok ? fork() : -1;
  if (child == 0) {
    close(alias);
    close(ready[0]);
    struct sigaction sa = {.sa_handler = flock_alarm};
    struct itimerval timer = {.it_value.tv_usec = 20000};
    int interrupted = sigaction(SIGALRM, &sa, NULL) == 0 && setitimer(ITIMER_REAL, &timer, NULL) == 0 &&
                      flock(b, LOCK_SH) == -1 && errno == EINTR;
    if (!interrupted || write(ready[1], "r", 1) != 1) { _exit(1); }
    if (flock(b, LOCK_SH) != 0 || write(ready[1], "s", 1) != 1) { _exit(1); }
    _exit(flock(b, LOCK_EX) == 0 ? 0 : 1);
  }
  close(ready[1]);
  char byte;
  ok = child > 0 && read(ready[0], &byte, 1) == 1 && byte == 'r' && flock(alias, LOCK_SH) == 0 &&
       read(ready[0], &byte, 1) == 1 && byte == 's';
  if (alias >= 0) { close(alias); }
  int status = 0;
  if (child > 0) { ok = waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 && ok; }
  ok = ok && flock(b, LOCK_EX | LOCK_NB) == 0;
  close(b);
  close(ready[0]);
  printf("[spore] flock alias, conversion and interrupted wait: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int removal_errors(const char *base, int test_symlink) {
  if (mkdir(base, 0700) != 0) { return 0; }
  int dir = open(base, O_RDONLY | O_DIRECTORY);
  if (dir < 0) { return 0; }
  int fd = openat(dir, ".keep", O_CREAT | O_RDWR, 0600);
  int ok = fd >= 0 && write(fd, "X", 1) == 1 &&
           unlinkat(dir, ".keep", AT_REMOVEDIR) < 0 && errno == ENOTDIR &&
           unlinkat(dir, ".keep", 1) < 0 && errno == EINVAL &&
           unlinkat(dir, ".keep/", 0) < 0 && errno == ENOTDIR &&
           unlinkat(dir, ".", AT_REMOVEDIR) < 0 && errno == EINVAL &&
           unlinkat(dir, "..", AT_REMOVEDIR) < 0 && errno == ENOTEMPTY &&
           unlinkat(dir, "", AT_REMOVEDIR) < 0 && errno == ENOENT &&
           unlink(base) < 0 && errno == EISDIR && rmdir(base) < 0 && errno == ENOTEMPTY;
  if (fd >= 0) { close(fd); }
  if (test_symlink) {
    ok = ok && symlinkat(".keep", dir, "link") == 0 &&
         unlinkat(dir, "link", AT_REMOVEDIR) < 0 && errno == ENOTDIR &&
         unlinkat(dir, "link/", 0) < 0 && errno == ENOTDIR && unlinkat(dir, "link", 0) == 0;
  }
  fd = openat(dir, ".keep", O_RDONLY);
  char marker = 0;
  ok = ok && fd >= 0 && read(fd, &marker, 1) == 1 && marker == 'X';
  if (fd >= 0) { close(fd); }
  ok = ok && unlinkat(dir, ".keep", 0) == 0 && rmdir(base) == 0;
  close(dir);
  printf("[spore] removal semantics %s: %s\n", base, ok ? "PASS" : "FAIL");
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
  errno = 0;
  if (syscall(SYS_getdents64, dfd, dents, 1) != -1 || errno != EINVAL) { ok = 0; }
  long dent_bytes = syscall(SYS_getdents64, dfd, dents, sizeof(dents));
  int saw_b = 0;
  for (long off = 0; off < dent_bytes;) {
    struct linux_dirent64 *d = (struct linux_dirent64 *)(dents + off);
    if (strcmp(d->d_name, "b") == 0 && d->d_type == DT_REG) { saw_b = 1; }
    off += d->d_reclen;
  }
  if (dfd >= 0) { close(dfd); }
  if (!saw_b) { ok = 0; }
  if (!removal_errors("/unlink-test", 1) || !removal_errors("/tmp/unlink-test", 0) || !flock_regression() || !inotify_shared_watch() || !epoll_watch_lifetime() || !fifo_without_reader() || !pathname_page_boundary() ||
      !getdents_proc_batches() || !getdents_fault_resume()) {
    ok = 0;
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
