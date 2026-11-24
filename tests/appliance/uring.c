#define _GNU_SOURCE
#ifdef NDEBUG
#error This guest test requires assertions
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void *wait_for_cqe(void *arg) {
  assert(syscall(SYS_io_uring_enter, *(int *)arg, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0) == 0);
  return NULL;
}

static void exercise(unsigned flags) {
  struct io_uring_params p = {.flags = flags};
  int fd = syscall(SYS_io_uring_setup, 4, &p);
  assert(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
  _Alignas(struct io_uring_probe) unsigned char
    storage[sizeof(struct io_uring_probe) + 64 * sizeof(struct io_uring_probe_op)] = {0};
  struct io_uring_probe *probe = (void *)storage;
  assert(syscall(SYS_io_uring_register, fd, IORING_REGISTER_PROBE, probe, 64) == 0);
  assert(probe->ops_len > IORING_OP_EPOLL_CTL);
  assert(probe->ops[IORING_OP_EPOLL_CTL].flags & IO_URING_OP_SUPPORTED);
  size_t ring_size = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  size_t array_end = p.sq_off.array + p.sq_entries * sizeof(unsigned);
  if (array_end > ring_size) { ring_size = array_end; }
  char *ring = mmap(NULL, ring_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_SQ_RING);
  size_t sqe_size = p.sq_entries * sizeof(struct io_uring_sqe);
  struct io_uring_sqe *sq = mmap(NULL, sqe_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, IORING_OFF_SQES);
  assert(ring != MAP_FAILED && sq != MAP_FAILED);
  struct io_uring_cqe *cq = (void *)(ring + p.cq_off.cqes);
  unsigned *tail = (void *)(ring + p.sq_off.tail), *head = (void *)(ring + p.cq_off.head);
  int ep = epoll_create1(0), event = eventfd(1, 0);
  struct epoll_event watch = {.events = EPOLLIN, .data.u64 = 1234};
  assert(ep >= 0 && event >= 0);
  pthread_t waiter;
  assert(!pthread_create(&waiter, NULL, wait_for_cqe, &fd));
  for (unsigned i = 0; i < 40; ++i) {
    unsigned slot = i % p.sq_entries;
    if (!(flags & IORING_SETUP_NO_SQARRAY)) {
      unsigned *array = (void *)(ring + p.sq_off.array);
      array[slot] = p.sq_entries - 1 - slot;
      slot = array[slot];
    }
    sq[slot] = (struct io_uring_sqe){.opcode = IORING_OP_NOP, .user_data = i};
    if (!i) {
      sq[slot].opcode = IORING_OP_EPOLL_CTL;
      sq[slot].fd = ep;
      sq[slot].off = event;
      sq[slot].len = EPOLL_CTL_ADD;
      sq[slot].addr = (uintptr_t)&watch;
    }
    __atomic_store_n(tail, i + 1, __ATOMIC_RELEASE);
    assert(syscall(SYS_io_uring_enter, fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0) == 1);
    if (!i) {
      struct epoll_event ready;
      assert(!pthread_join(waiter, NULL));
      assert(epoll_wait(ep, &ready, 1, 0) == 1 && ready.data.u64 == watch.data.u64);
    }
    struct pollfd ready = {.fd = fd, .events = POLLIN};
    assert(poll(&ready, 1, 0) == 1 && ready.revents == POLLIN);
    assert(cq[i % p.cq_entries].res == 0 && cq[i % p.cq_entries].user_data == i);
    __atomic_store_n(head, i + 1, __ATOMIC_RELEASE);
  }
  assert(close(fd) == 0 && cq[39 % p.cq_entries].user_data == 39);
  assert(munmap(sq, sqe_size) == 0 && munmap(ring, ring_size) == 0);
  close(event);
  close(ep);
}

static volatile sig_atomic_t alarm_seen;

static void on_alarm(int signal) {
  alarm_seen = signal;
}

static void interrupted_epoll(void) {
  struct sigaction action = {.sa_handler = on_alarm};
  assert(sigemptyset(&action.sa_mask) == 0 && sigaction(SIGALRM, &action, NULL) == 0);
  int ep = epoll_create1(0);
  assert(ep >= 0);
  struct itimerval timer = {.it_value.tv_usec = 50000};
  assert(setitimer(ITIMER_REAL, &timer, NULL) == 0);
  struct epoll_event event;
  assert(epoll_wait(ep, &event, 1, 3000) == -1 && errno == EINTR);
  assert(alarm_seen == SIGALRM && close(ep) == 0);
}

struct test_ring {
  int fd;
  struct io_uring_params p;
  char *map;
  size_t bytes;
  struct io_uring_sqe *sq;
  unsigned tail, head;
};

static void test_ring_open(struct test_ring *r) {
  r->p.flags = IORING_SETUP_NO_SQARRAY;
  r->fd = syscall(SYS_io_uring_setup, 8, &r->p);
  assert(r->fd >= 0);
  r->bytes = r->p.cq_off.cqes + r->p.cq_entries * sizeof(struct io_uring_cqe);
  r->map = mmap(NULL, r->bytes, 3, MAP_SHARED, r->fd, 0);
  r->sq = mmap(NULL, r->p.sq_entries * sizeof(*r->sq), 3, MAP_SHARED, r->fd, IORING_OFF_SQES);
  assert(r->map != MAP_FAILED && r->sq != MAP_FAILED);
}

static void queue(struct test_ring *r, struct io_uring_sqe e) {
  r->sq[r->tail++ % r->p.sq_entries] = e;
  __atomic_store_n((unsigned *)(r->map + r->p.sq_off.tail), r->tail, __ATOMIC_RELEASE);
}

static struct io_uring_cqe completion(struct test_ring *r) {
  unsigned *tail = (unsigned *)(r->map + r->p.cq_off.tail);
  if (r->head == __atomic_load_n(tail, __ATOMIC_ACQUIRE)) {
    assert(syscall(SYS_io_uring_enter, r->fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0) == 0);
  }
  assert(r->head != __atomic_load_n(tail, __ATOMIC_ACQUIRE));
  struct io_uring_cqe *cq = (void *)(r->map + r->p.cq_off.cqes);
  struct io_uring_cqe e = cq[r->head++ % r->p.cq_entries];
  __atomic_store_n((unsigned *)(r->map + r->p.cq_off.head), r->head, __ATOMIC_RELEASE);
  return e;
}

static int issue(struct test_ring *r, struct io_uring_sqe e) {
  queue(r, e);
  assert(syscall(SYS_io_uring_enter, r->fd, 1, 0, 0, NULL, 0) == 1);
  return completion(r).res;
}

static void files_and_events(void) {
  struct test_ring r = {0};
  test_ring_open(&r);
  int file = issue(&r, (struct io_uring_sqe){.opcode = IORING_OP_OPENAT,
                                             .fd = AT_FDCWD,
                                             .addr = (uintptr_t)"/tmp/ring-io",
                                             .open_flags = O_CREAT | O_RDWR | O_TRUNC,
                                             .len = 0600});
  assert(file >= 0);
  char text[] = "ring buffers";
  assert(
    issue(&r, (struct io_uring_sqe){
                .opcode = IORING_OP_WRITE, .fd = file, .addr = (uintptr_t)text, .len = sizeof(text)}) == sizeof(text));
  assert(syscall(SYS_io_uring_register, r.fd, IORING_REGISTER_FILES, &file, 1) == 0);
  assert(close(file) == 0);
  int pipefd[2];
  assert(pipe(pipefd) == 0);
  char *buf = mmap(NULL, 8192, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(buf != MAP_FAILED);
  struct iovec v = {.iov_base = buf + 4092, .iov_len = sizeof(text)};
  assert(syscall(SYS_io_uring_register, r.fd, IORING_REGISTER_BUFFERS, &v, 1) == 0);
  assert(issue(&r, (struct io_uring_sqe){.opcode = IORING_OP_READ_FIXED,
                                         .flags = IOSQE_FIXED_FILE,
                                         .fd = 0,
                                         .addr = (uintptr_t)v.iov_base,
                                         .len = sizeof(text)}) == sizeof(text));
  assert(!memcmp(v.iov_base, text, sizeof(text)));
  assert(munmap(buf, 8192) == 0);
  assert(issue(&r, (struct io_uring_sqe){.opcode = IORING_OP_WRITE_FIXED,
                                         .flags = IOSQE_FIXED_FILE,
                                         .fd = 0,
                                         .off = sizeof(text),
                                         .addr = (uintptr_t)v.iov_base,
                                         .len = sizeof(text)}) == sizeof(text));
  assert(syscall(SYS_io_uring_register, r.fd, IORING_UNREGISTER_BUFFERS, NULL, 0) == 0);
  int installed =
    issue(&r, (struct io_uring_sqe){.opcode = IORING_OP_FIXED_FD_INSTALL, .flags = IOSQE_FIXED_FILE, .fd = 0});
  char readback[sizeof(text)] = {0};
  assert(installed >= 0 && pread(installed, readback, sizeof(text), sizeof(text)) == sizeof(text) &&
         !memcmp(text, readback, sizeof(text)));
  assert(syscall(SYS_io_uring_register, r.fd, IORING_UNREGISTER_FILES, NULL, 0) == 0);
  int event = eventfd(0, EFD_NONBLOCK);
  assert(event >= 0 && syscall(SYS_io_uring_register, r.fd, IORING_REGISTER_EVENTFD, &event, 1) == 0);
  char got = 0;
  queue(&r, (struct io_uring_sqe){
              .opcode = IORING_OP_READ, .fd = pipefd[0], .addr = (uintptr_t)&got, .len = 1, .user_data = 123});
  assert(syscall(SYS_io_uring_enter, r.fd, 1, 0, 0, NULL, 0) == 1);
  assert(close(pipefd[0]) == 0);
  int reused = eventfd(1, 0);
  assert(reused == pipefd[0]);
  assert(write(pipefd[1], "x", 1) == 1);
  struct io_uring_cqe c = completion(&r);
  assert(c.user_data == 123 && c.res == 1 && got == 'x');
  uint64_t notices = 0;
  assert(read(event, &notices, 8) == 8 && notices == 1);
  queue(&r, (struct io_uring_sqe){.opcode = IORING_OP_POLL_ADD, .fd = event, .poll_events = POLLIN, .user_data = 456});
  queue(&r, (struct io_uring_sqe){.opcode = IORING_OP_POLL_REMOVE, .addr = 456, .user_data = 789});
  assert(syscall(SYS_io_uring_enter, r.fd, 2, 0, 0, NULL, 0) == 2);
  int canceled = 0, removed = 0;
  for (int i = 0; i < 2; i++) {
    c = completion(&r);
    canceled += c.user_data == 456 && c.res == -ECANCELED;
    removed += c.user_data == 789 && c.res == 0;
  }
  assert(canceled == 1 && removed == 1);
  struct timespec ts = {.tv_nsec = 15000000};
  assert(issue(&r, (struct io_uring_sqe){.opcode = IORING_OP_TIMEOUT, .fd = -1, .addr = (uintptr_t)&ts, .len = 1}) ==
         -ETIME);
  assert(close(installed) == 0 && close(event) == 0 && close(reused) == 0 && close(pipefd[1]) == 0);
  assert(close(r.fd) == 0);
  assert(munmap(r.map, r.bytes) == 0 && munmap(r.sq, r.p.sq_entries * sizeof(*r.sq)) == 0);
}

static void overflow_and_tags(void) {
  struct test_ring r = {0};
  test_ring_open(&r);
  for (unsigned batch = 0; batch < 3; ++batch) {
    for (unsigned i = 0; i < 8; ++i) {
      queue(&r, (struct io_uring_sqe){.opcode = IORING_OP_NOP, .user_data = batch * 8 + i});
    }
    assert(syscall(SYS_io_uring_enter, r.fd, 8, 0, 0, NULL, 0) == 8);
  }
  for (unsigned i = 0; i < 24; ++i) {
    struct io_uring_cqe c = completion(&r);
    assert(c.res == 0 && c.user_data == i);
  }
  int pipes[2];
  assert(pipe(pipes) == 0);
  uint64_t tag = 999;
  struct io_uring_rsrc_register reg = {.nr = 1, .data = (uintptr_t)&pipes[0], .tags = (uintptr_t)&tag};
  assert(syscall(SYS_io_uring_register, r.fd, IORING_REGISTER_FILES2, &reg, sizeof(reg)) == 0);
  char ch = 0;
  queue(&r, (struct io_uring_sqe){.opcode = IORING_OP_READ,
                                  .flags = IOSQE_FIXED_FILE,
                                  .fd = 0,
                                  .addr = (uintptr_t)&ch,
                                  .len = 1,
                                  .user_data = 888});
  assert(syscall(SYS_io_uring_enter, r.fd, 1, 0, 0, NULL, 0) == 1);
  assert(syscall(SYS_io_uring_register, r.fd, IORING_UNREGISTER_FILES, NULL, 0) == 0);
  assert(*(unsigned *)(r.map + r.p.cq_off.tail) == r.head);
  assert(write(pipes[1], "t", 1) == 1);
  struct io_uring_cqe c = completion(&r);
  assert(c.user_data == 888 && c.res == 1 && ch == 't');
  c = completion(&r);
  assert(c.user_data == 999 && !c.res);

  struct timespec ts = {.tv_nsec = 15000000};
  struct io_uring_getevents_arg arg = {.ts = (uintptr_t)&ts};
  errno = 0;
  assert(syscall(SYS_io_uring_enter, r.fd, 0, 1, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG, &arg, sizeof(arg)) ==
           -1 &&
         errno == ETIME);
  assert(close(pipes[0]) == 0 && close(pipes[1]) == 0 && close(r.fd) == 0);
  assert(munmap(r.map, r.bytes) == 0 && munmap(r.sq, r.p.sq_entries * sizeof(*r.sq)) == 0);
}

static void provided_buffers_and_links(void) {
  struct test_ring r = {0};
  test_ring_open(&r);
  int p[2];
  assert(pipe(p) == 0);
  char buffers[2][4] = {0};
  assert(issue(&r, (struct io_uring_sqe){.opcode = IORING_OP_PROVIDE_BUFFERS,
                                         .fd = 2,
                                         .addr = (uintptr_t)buffers,
                                         .len = 4,
                                         .off = 10,
                                         .buf_group = 5}) == 0);
  queue(&r, (struct io_uring_sqe){.opcode = IORING_OP_READ,
                                  .flags = IOSQE_BUFFER_SELECT | IOSQE_IO_LINK,
                                  .buf_group = 5,
                                  .fd = p[0],
                                  .user_data = 1});
  queue(&r, (struct io_uring_sqe){.opcode = IORING_OP_NOP, .user_data = 2});
  assert(syscall(SYS_io_uring_enter, r.fd, 2, 0, 0, NULL, 0) == 2);
  assert(*(unsigned *)(r.map + r.p.cq_off.tail) == r.head);
  assert(write(p[1], "data", 4) == 4);
  struct io_uring_cqe c = completion(&r);
  assert(c.user_data == 1 && c.res == 4 && c.flags == (IORING_CQE_F_BUFFER | (10u << 16)) &&
         !memcmp(buffers[0], "data", 4));
  c = completion(&r);
  assert(c.user_data == 2 && c.res == 0);
  assert(issue(&r, (struct io_uring_sqe){.opcode = IORING_OP_REMOVE_BUFFERS, .fd = 4, .buf_group = 5}) == 1);

  struct io_uring_buf_ring *br = mmap(NULL, 4096, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(br != MAP_FAILED);
  struct io_uring_buf_reg reg = {.ring_addr = (uintptr_t)br, .ring_entries = 8, .bgid = 8};
  assert(syscall(SYS_io_uring_register, r.fd, IORING_REGISTER_PBUF_RING, &reg, 1) == 0);
  br->bufs[0] = (struct io_uring_buf){.addr = (uintptr_t)buffers[1], .len = 4, .bid = 73};
  __atomic_store_n(&br->tail, 1, __ATOMIC_RELEASE);
  assert(write(p[1], "read", 4) == 4);
  queue(&r, (struct io_uring_sqe){.opcode = IORING_OP_READ, .fd = p[0], .flags = IOSQE_BUFFER_SELECT, .buf_group = 8});
  assert(syscall(SYS_io_uring_enter, r.fd, 1, 0, 0, NULL, 0) == 1);
  c = completion(&r);
  assert(c.res == 4 && c.flags == (IORING_CQE_F_BUFFER | (73u << 16)) && !memcmp(buffers[1], "read", 4));
  reg = (struct io_uring_buf_reg){.bgid = 8};
  assert(syscall(SYS_io_uring_register, r.fd, IORING_UNREGISTER_PBUF_RING, &reg, 1) == 0);
  assert(munmap(br, 4096) == 0);
  struct timespec ts = {.tv_nsec = 15000000};
  queue(&r, (struct io_uring_sqe){
              .opcode = IORING_OP_POLL_ADD, .fd = p[0], .poll_events = POLLIN, .flags = IOSQE_IO_LINK, .user_data = 3});
  queue(&r, (struct io_uring_sqe){
              .opcode = IORING_OP_LINK_TIMEOUT, .fd = -1, .addr = (uintptr_t)&ts, .len = 1, .user_data = 4});
  assert(syscall(SYS_io_uring_enter, r.fd, 2, 0, 0, NULL, 0) == 2);
  int canceled = 0, expired = 0;
  for (int i = 0; i < 2; ++i) {
    c = completion(&r);
    canceled += c.user_data == 3 && c.res == -ECANCELED;
    expired += c.user_data == 4 && c.res == -ETIME;
  }
  assert(canceled == 1 && expired == 1);
  close(p[0]);
  close(p[1]);
  close(r.fd);
  munmap(r.map, r.bytes);
  munmap(r.sq, r.p.sq_entries * sizeof(*r.sq));
}

static void message_ring(void) {
  struct test_ring src = {0}, dst = {0};
  test_ring_open(&src);
  test_ring_open(&dst);
  assert(issue(&src, (struct io_uring_sqe){.opcode = IORING_OP_MSG_RING, .fd = dst.fd, .off = 0x1234, .len = 17}) == 0);
  struct io_uring_cqe c = completion(&dst);
  assert(c.user_data == 0x1234 && c.res == 17);
  struct io_uring_rsrc_register sparse = {.nr = 4, .flags = IORING_RSRC_REGISTER_SPARSE};
  assert(syscall(SYS_io_uring_register, src.fd, IORING_REGISTER_FILES2, &sparse, sizeof(sparse)) == 0);
  assert(issue(&src, (struct io_uring_sqe){.opcode = IORING_OP_OPENAT,
                                           .fd = AT_FDCWD,
                                           .addr = (uintptr_t)"/tmp/ring-direct",
                                           .len = 0600,
                                           .open_flags = O_CREAT | O_RDWR,
                                           .file_index = 3}) == 0);
  int fd = issue(&src, (struct io_uring_sqe){.opcode = IORING_OP_FIXED_FD_INSTALL, .flags = IOSQE_FIXED_FILE, .fd = 2});
  assert(fd >= 0 && write(fd, "f", 1) == 1);
  assert(issue(&src, (struct io_uring_sqe){.opcode = IORING_OP_CLOSE, .file_index = 3}) == 0);
  assert(pread(fd, &c, 1, 0) == 1 && *(char *)&c == 'f');
  close(fd);
  close(src.fd);
  close(dst.fd);
  munmap(src.map, src.bytes);
  munmap(dst.map, dst.bytes);
  munmap(src.sq, src.p.sq_entries * sizeof(*src.sq));
  munmap(dst.sq, dst.p.sq_entries * sizeof(*dst.sq));
}

int main(void) {
  message_ring();
  provided_buffers_and_links();
  overflow_and_tags();
  files_and_events();
  int status;
  assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
  const struct timespec delay = {.tv_nsec = 20000000};
  assert(nanosleep(&delay, NULL) == 0);
  interrupted_epoll();
  exercise(0);
  exercise(IORING_SETUP_NO_SQARRAY);
  return 0;
}
