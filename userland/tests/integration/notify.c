#include "integration.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct event_batch {
  _Alignas(struct inotify_event) char bytes[2048];
  size_t size, offset;
};

static int read_batch(int fd, struct event_batch *batch) {
  int bytes;
  if (ioctl(fd, FIONREAD, &bytes) || bytes < 1 || (size_t)bytes > sizeof(batch->bytes)) { return 0; }
  ssize_t n = read(fd, batch->bytes, sizeof(batch->bytes));
  batch->offset = 0;
  batch->size = n > 0 ? (size_t)n : 0;
  return n == bytes;
}

static const struct inotify_event *next_event(struct event_batch *batch, int wd, uint32_t mask, const char *name) {
  if (batch->size - batch->offset < sizeof(struct inotify_event)) { return NULL; }
  const struct inotify_event *e = (const void *)(batch->bytes + batch->offset);
  size_t size = sizeof(*e) + e->len;
  if (size > batch->size - batch->offset || e->wd != wd || e->mask != mask ||
      (name ? !e->len || !memchr(e->name, 0, e->len) || strcmp(name, e->name) : e->len != 0)) { return NULL; }
  batch->offset += size;
  return e;
}

static int directory_events(const char *path) {
  int saved = open(".", O_RDONLY | O_DIRECTORY);
  if (saved < 0 || mkdir(path, 0700) || chdir(path)) { return 0; }
  int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC), ep = epoll_create1(0);
  int wd = inotify_add_watch(ifd, ".", IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVE | IN_DELETE);
  struct epoll_event registration = {.events = EPOLLIN, .data.fd = ifd}, ready;
  int ok = wd > 0 && epoll_ctl(ep, EPOLL_CTL_ADD, ifd, &registration) == 0;
  int file = open("a", O_CREAT | O_RDWR, 0600);
  ok = ok && file >= 0 && write(file, "1", 1) == 1 && write(file, "2", 1) == 1 && rename("a", "b") == 0 &&
       write(file, "3", 1) == 1 && unlink("b") == 0 && write(file, "4", 1) == 1;
  if (file >= 0) { close(file); }
  struct event_batch batch;
  char tiny[1];
  ok = ok && epoll_wait(ep, &ready, 1, 0) == 1 && ready.data.fd == ifd && read(ifd, tiny, 1) == -1 && errno == EINVAL &&
       read_batch(ifd, &batch) && next_event(&batch, wd, IN_CREATE, "a") && next_event(&batch, wd, IN_MODIFY, "a");
  const struct inotify_event *from = ok ? next_event(&batch, wd, IN_MOVED_FROM, "a") : NULL;
  const struct inotify_event *to = from ? next_event(&batch, wd, IN_MOVED_TO, "b") : NULL;
  ok = to && from->cookie && from->cookie == to->cookie && next_event(&batch, wd, IN_MODIFY, "b") &&
       next_event(&batch, wd, IN_DELETE, "b") && next_event(&batch, wd, IN_MODIFY, "b") &&
       next_event(&batch, wd, IN_CLOSE_WRITE, "b") && batch.size == batch.offset && epoll_wait(ep, &ready, 1, 0) == 0;
  close(ep); close(ifd);
  fchdir(saved); close(saved); rmdir(path);
  printf("[spore] inotify directory %s: %s\n", path, ok ? "PASS" : "FAIL");
  return ok;
}

static int inode_events(void) {
  const char *path = "/tmp/notify-file", *alias = "/tmp/notify-link";
  int file = open(path, O_CREAT | O_RDWR, 0600), fd = inotify_init1(IN_NONBLOCK);
  int wd = inotify_add_watch(fd, path, IN_MODIFY);
  int ok = file >= 0 && wd > 0 && link(path, alias) == 0 &&
           inotify_add_watch(fd, alias, IN_MASK_CREATE | IN_OPEN) == -1 && errno == EEXIST &&
           inotify_add_watch(fd, alias, IN_MASK_ADD | IN_DELETE_SELF) == wd && unlink(path) == 0 && unlink(alias) == 0 &&
           write(file, "x", 1) == 1;
  if (file >= 0) { close(file); }
  struct event_batch batch;
  ok = ok && read_batch(fd, &batch) && next_event(&batch, wd, IN_MODIFY, NULL) &&
       next_event(&batch, wd, IN_DELETE_SELF, NULL) && next_event(&batch, wd, IN_IGNORED, NULL) && batch.offset == batch.size;
  close(fd);
  printf("[spore] inotify inode aliases and final unlink: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int blocked_reader(void) {
  int fd = inotify_init1(0);
  int wd = inotify_add_watch(fd, "/tmp", IN_CREATE | IN_ONESHOT);
  if (wd < 0) { return 0; }
  pid_t child = fork();
  if (!child) {
    usleep(20000);
    int f = open("/tmp/notify-one", O_CREAT | O_EXCL | O_WRONLY, 0600);
    _exit(f < 0);
  }
  _Alignas(struct inotify_event) char data[64];
  ssize_t n = child > 0 ? read(fd, data, sizeof(data)) : -1;
  int status;
  int ok = n > 0 && waitpid(child, &status, 0) == child && status == 0;
  if (ok) {
    struct event_batch batch = {.size = (size_t)n};
    memcpy(batch.bytes, data, (size_t)n);
    ok = next_event(&batch, wd, IN_CREATE, "notify-one") && next_event(&batch, wd, IN_IGNORED, NULL) &&
         batch.offset == batch.size;
  }
  close(fd); unlink("/tmp/notify-one");
  printf("[spore] inotify blocked one-shot: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

int integration_notify_suite(void) {
  return directory_events("/notify-check") && directory_events("/tmp/notify-check") && inode_events() && blocked_reader();
}
