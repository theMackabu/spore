#include "integration.h"

#include "dirent64.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/select.h>

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

static int eventfd_counter(void) {
  int fd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
  if (fd < 0) { return 0; }
  eventfd_t value;
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLOUT};
  int ok = eventfd_read(fd, &value) == -1 && errno == EAGAIN &&
           eventfd_write(fd, UINT64_MAX) == -1 && errno == EINVAL &&
           eventfd_write(fd, UINT64_MAX - 1) == 0 && poll(&pfd, 1, 0) == 1 && pfd.revents == POLLIN &&
           eventfd_write(fd, 1) == -1 && errno == EAGAIN && eventfd_read(fd, &value) == 0 && value == 1 &&
           poll(&pfd, 1, 0) == 1 && pfd.revents == (POLLIN | POLLOUT);
  close(fd);

  fd = eventfd(0, 0);
  if (fd < 0) { return 0; }
  pid_t child = fork();
  if (child == 0) {
    integration_sleep_ms(25);
    _exit(eventfd_write(fd, UINT64_MAX - 1) != 0);
  }
  int status;
  ok = child > 0 && eventfd_read(fd, &value) == 0 && value == UINT64_MAX - 1 && ok;
  if (child > 0) { ok = waitpid(child, &status, 0) == child && status == 0 && ok; }
  if (!ok || eventfd_write(fd, UINT64_MAX - 1) != 0) {
    close(fd);
    return 0;
  }
  child = fork();
  if (child == 0) {
    integration_sleep_ms(25);
    _exit(eventfd_read(fd, &value) != 0 || value != UINT64_MAX - 1);
  }
  ok = child > 0 && eventfd_write(fd, 3) == 0 && eventfd_read(fd, &value) == 0 && value == 3;
  if (child > 0) { ok = waitpid(child, &status, 0) == child && status == 0 && ok; }
  close(fd);
  printf("[spore] eventfd capacity, semaphore and blocked transfers: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int high_fd_limit(void) {
  int counter = eventfd(1, EFD_NONBLOCK), fd = fcntl(counter, F_DUPFD_CLOEXEC, 900);
  if (fd < 0) { return 0; }
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) { return 0; }
  limit.rlim_cur = 16;
  if (setrlimit(RLIMIT_NOFILE, &limit) != 0) { return 0; }
  fd_set reads, writes;
  FD_ZERO(&reads); FD_SET(fd, &reads);
  FD_ZERO(&writes); FD_SET(fd, &writes);
  struct timeval now = {0};
  if (select(fd + 1, &reads, &writes, NULL, &now) != 2 || !FD_ISSET(fd, &reads) || !FD_ISSET(fd, &writes) ||
      fcntl(fd, F_GETFD) != FD_CLOEXEC || dup2(counter, fd + 1) != -1 || errno != EBADF) { return 0; }
  pid_t child = fork();
  if (!child) {
    struct rlimit inherited;
    eventfd_t value;
    _exit(getrlimit(RLIMIT_NOFILE, &inherited) || inherited.rlim_cur != 16 ||
          eventfd_read(fd, &value) || value != 1);
  }
  int status;
  if (child < 0 || waitpid(child, &status, 0) != child || status != 0) { return 0; }
  close(fd);
  FD_ZERO(&reads); FD_SET(fd, &reads);
  return select(fd + 1, &reads, NULL, NULL, &now) == -1 && errno == EBADF;
}

static int fd_limit_regression(void) {
  pid_t child = fork();
  if (!child) { _exit(!high_fd_limit()); }
  int status;
  int ok = child > 0 && waitpid(child, &status, 0) == child && status == 0;
  printf("[spore] sparse descriptors and inherited limits: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int epoll_watch_lifetime(void) {
  int ep = epoll_create1(0), counter = eventfd(0, EFD_NONBLOCK), alias = dup(counter);
  struct epoll_event watch = {.events = EPOLLIN, .data.u64 = 42}, event;
  int ok = ep >= 0 && alias >= 0 && epoll_ctl(ep, EPOLL_CTL_MOD, counter, &watch) == -1 && errno == ENOENT &&
           epoll_ctl(ep, EPOLL_CTL_ADD, counter, &watch) == 0 &&
           epoll_ctl(ep, EPOLL_CTL_ADD, counter, &watch) == -1 && errno == EEXIST;
  close(counter);
  int replacement = eventfd(0, EFD_NONBLOCK);
  ok = ok && replacement == counter && eventfd_write(replacement, 1) == 0 && epoll_wait(ep, &event, 1, 0) == 0 &&
       eventfd_write(alias, 1) == 0 && epoll_wait(ep, &event, 1, 0) == 1 && event.data.u64 == 42;
  close(alias);
  ok = ok && epoll_wait(ep, &event, 1, 0) == 0 && epoll_ctl(ep, EPOLL_CTL_DEL, replacement, NULL) == -1 && errno == ENOENT;
  close(replacement); close(ep);
  return ok;
}

static int epoll_ready_queue(void) {
  int ep = epoll_create1(0), outer = epoll_create1(0), p[2];
  if (ep < 0 || outer < 0 || pipe(p)) { return 0; }
  struct epoll_event w = {.events = EPOLLIN, .data.u64 = 42}, e;
  int ok = epoll_ctl(outer, EPOLL_CTL_ADD, ep, &w) == 0;
  w.events |= EPOLLET;
  char byte;
  ok = ok && epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &w) == 0 && write(p[1], "ab", 2) == 2 &&
       epoll_wait(outer, &e, 1, 0) == 1 && epoll_wait(ep, &e, 1, 0) == 1 && read(p[0], &byte, 1) == 1 &&
       epoll_wait(ep, &e, 1, 0) == 0 && epoll_wait(outer, &e, 1, 0) == 0 && read(p[0], &byte, 1) == 1 &&
       write(p[1], "c", 1) == 1 && epoll_wait(ep, &e, 1, 0) == 1;
  w.events = EPOLLIN | EPOLLONESHOT;
  ok = ok && epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &w) == 0 && epoll_wait(ep, &e, 1, 0) == 1 &&
       epoll_wait(ep, &e, 1, 0) == 0 && epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &w) == 0 &&
       epoll_wait(ep, &e, 1, 0) == 1;
  int counter = eventfd(0, EFD_NONBLOCK);
  w.events = EPOLLIN | EPOLLET;
  ok = ok && counter >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, counter, &w) == 0 &&
       eventfd_write(counter, 1) == 0 && epoll_wait(ep, &e, 1, 0) == 1 && epoll_wait(ep, &e, 1, 0) == 0 &&
       eventfd_write(counter, 1) == 0 && epoll_wait(ep, &e, 1, 0) == 1;
  if (counter >= 0) { close(counter); }
  close(p[0]); close(p[1]); close(outer); close(ep);
  printf("[spore] nested epoll, edge transitions and one-shot rearm: %s\n", ok ? "PASS" : "FAIL");
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

static int disk_fifo(void) {
  const char *name = "/spore-fifo", *link_name = "/spore-fifo-link";
  if (mkfifo(name, 0600) || link(name, link_name)) { return 0; }
  struct stat named, opened;
  int rd = open(name, O_RDONLY | O_NONBLOCK);
  char c;
  int ok = rd >= 0 && read(rd, &c, 1) == 0 && fstat(rd, &opened) == 0 && stat(link_name, &named) == 0 &&
           S_ISFIFO(named.st_mode) && named.st_ino == opened.st_ino && named.st_dev == opened.st_dev;
  int both = open(link_name, O_RDWR | O_NONBLOCK);
  ok = ok && both >= 0 && unlink(name) == 0 && unlink(link_name) == 0 && write(both, "ab", 2) == 2 &&
       read(rd, &c, 1) == 1 && c == 'a' && read(both, &c, 1) == 1 && c == 'b' &&
       fstat(rd, &opened) == 0 && opened.st_nlink == 0;
  if (both >= 0) { close(both); }
  struct pollfd p = {.fd = rd, .events = POLLIN};
  ok = ok && poll(&p, 1, 0) == 1 && (p.revents & POLLHUP) && read(rd, &c, 1) == 0;
  if (rd >= 0) { close(rd); }
  printf("[spore] ext2 FIFO aliases and read/write endpoints: %s\n", ok ? "PASS" : "FAIL");
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

static int rename_entries(const char *base) {
  if (mkdir(base, 0700) != 0) { return 0; }
  int dir = open(base, O_RDONLY | O_DIRECTORY);
  if (dir < 0) { return 0; }
  int old = openat(dir, "old", O_CREAT | O_EXCL | O_RDWR, 0600);
  int target = openat(dir, "target", O_CREAT | O_EXCL | O_RDWR, 0600);
  char byte;
  struct stat st;
  int ok = old >= 0 && target >= 0 && write(old, "A", 1) == 1 && write(target, "B", 1) == 1;
  ok = ok && syscall(SYS_renameat2, dir, "old", dir, "target", 1) == -1 && errno == EEXIST &&
       renameat(dir, "old", dir, "target") == 0 && pread(target, &byte, 1, 0) == 1 && byte == 'B' &&
       fstat(target, &st) == 0 && st.st_nlink == 0;
  int moved = openat(dir, "target", O_RDONLY);
  ok = ok && moved >= 0 && read(moved, &byte, 1) == 1 && byte == 'A' && mkdirat(dir, "tree", 0700) == 0 &&
       mkdirat(dir, "tree/child", 0700) == 0 && renameat(dir, "tree", dir, "tree/child/loop") == -1 && errno == EINVAL &&
       renameat(dir, "target", dir, "tree") == -1 && errno == EISDIR &&
       renameat(dir, "tree", dir, "target") == -1 && errno == ENOTDIR &&
       syscall(SYS_renameat2, dir, "tree", dir, "renamed", 1) == 0;
  if (moved >= 0) { close(moved); }
  if (old >= 0) { close(old); }
  if (target >= 0) { close(target); }
  unlinkat(dir, "renamed/child", AT_REMOVEDIR);
  unlinkat(dir, "renamed", AT_REMOVEDIR);
  unlinkat(dir, "target", 0);
  close(dir);
  rmdir(base);
  printf("[spore] rename replacement and ancestry %s: %s\n", base, ok ? "PASS" : "FAIL");
  return ok;
}

static int open_unlinked_ext2(void) {
  const char *path = "/open-unlinked";
  int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) { return 0; }
  int second = open(path, O_RDONLY);
  char byte = 0;
  struct stat original, replacement;
  int ok = second >= 0 && write(fd, "A", 1) == 1 && pread(second, &byte, 1, 0) == 1 && byte == 'A';
  char *mapping = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
  ok = ok && mapping != MAP_FAILED && unlink(path) == 0 && fstat(fd, &original) == 0 && !original.st_nlink;
  int other = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
  ok = ok && other >= 0 && write(other, "B", 1) == 1 && fstat(other, &replacement) == 0 &&
       replacement.st_ino != original.st_ino && pread(fd, &byte, 1, 0) == 1 && byte == 'A';
  close(fd);
  if (second >= 0) { close(second); }
  if (mapping != MAP_FAILED) {
    char *target = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (target != MAP_FAILED) {
      char *moved = mremap(mapping, 4096, 4096, MREMAP_MAYMOVE | MREMAP_FIXED, target);
      if (moved != MAP_FAILED) { mapping = moved; }
      else { munmap(target, 4096); }
      ok = ok && moved == target;
    } else { ok = 0; }
    ok = ok && mapping[0] == 'A';
    munmap(mapping, 4096);
  }
  if (other >= 0) { close(other); }
  unlink(path);
  printf("[spore] unlinked ext2 file and mapping: %s\n", ok ? "PASS" : "FAIL");
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
  if (!rename_entries("/rename-check") || !rename_entries("/tmp/rename-check") || !open_unlinked_ext2() || !removal_errors("/unlink-test", 1) || !removal_errors("/tmp/unlink-test", 0) || !flock_regression() || !inotify_shared_watch() || !integration_notify_suite() || !fd_limit_regression() || !eventfd_counter() || !epoll_watch_lifetime() || !epoll_ready_queue() || !fifo_without_reader() || !disk_fifo() || !pathname_page_boundary() ||
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
