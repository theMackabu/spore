#define _GNU_SOURCE
#ifdef NDEBUG
#error This guest test requires assertions
#endif
#include <assert.h>
#include <errno.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/time.h>
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

static void on_alarm(int signal) { alarm_seen = signal; }

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

int main(void) {
  int status;
  assert(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
  const struct timespec delay = {.tv_nsec = 20000000};
  assert(nanosleep(&delay, NULL) == 0);
  interrupted_epoll();
  exercise(0);
  exercise(IORING_SETUP_NO_SQARRAY);
  return 0;
}
