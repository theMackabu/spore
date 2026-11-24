#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifdef NDEBUG
#error This benchmark validates completions
#endif

enum { DEPTH = 64, REQUESTS = 131072, BYTES = 64 };

static uint64_t nanoseconds(void) {
  struct timespec ts;
  assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int main(void) {
  struct io_uring_params p = {.flags = IORING_SETUP_NO_SQARRAY};
  int fd = syscall(SYS_io_uring_setup, DEPTH, &p);
  assert(fd >= 0);
  char *map = mmap(NULL, p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe), 3, MAP_SHARED, fd, 0);
  struct io_uring_sqe *sq = mmap(NULL, DEPTH * sizeof(*sq), 3, MAP_SHARED, fd, IORING_OFF_SQES);
  assert(map != MAP_FAILED && sq != MAP_FAILED);
  struct io_uring_cqe *cq = (void *)(map + p.cq_off.cqes);
  unsigned *tail = (void *)(map + p.sq_off.tail), *head = (void *)(map + p.cq_off.head);
  char *data = mmap(NULL, DEPTH * BYTES, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(data != MAP_FAILED);
  memset(data, 'x', DEPTH * BYTES);
  int file = open("/tmp/uring-bench", O_CREAT | O_RDWR, 0600);
  assert(file >= 0 && write(file, data, DEPTH * BYTES) == DEPTH * BYTES);
  struct iovec v = {data, DEPTH * BYTES};
  assert(syscall(SYS_io_uring_register, fd, IORING_REGISTER_BUFFERS, &v, 1) == 0);
  assert(syscall(SYS_io_uring_register, fd, IORING_REGISTER_FILES, &file, 1) == 0);
  unsigned count = 0;
  for (unsigned batch = 1; batch <= DEPTH; batch *= 8) {
    uint64_t start = nanoseconds();
    for (unsigned i = 0; i < REQUESTS; i += batch) {
      count += batch;
      __atomic_store_n(tail, count, __ATOMIC_RELEASE);
      assert(syscall(SYS_io_uring_enter, fd, batch, 0, 0, NULL, 0) == batch);
      __atomic_store_n(head, count, __ATOMIC_RELEASE);
    }
    printf("NOP batch %u: %.2f ns/request\n", batch, (nanoseconds() - start) / (double)REQUESTS);
  }
  const char *names[] = {"pread", "ring read", "fixed ring read"};
  for (int mode = 0; mode < 3; ++mode) {
    for (unsigned j = 0; j < DEPTH; ++j) {
      sq[j] = (struct io_uring_sqe){.opcode = mode == 2 ? IORING_OP_READ_FIXED : IORING_OP_READ,
                                    .flags = mode == 2 ? IOSQE_FIXED_FILE : 0,
                                    .fd = mode == 2 ? 0 : file,
                                    .addr = (uintptr_t)data + j * BYTES,
                                    .off = j * BYTES,
                                    .len = BYTES};
    }
    uint64_t start = nanoseconds();
    for (unsigned i = 0; i < REQUESTS; i += DEPTH) {
      if (!mode) {
        for (unsigned j = 0; j < DEPTH; ++j) {
          assert(pread(file, data + j * BYTES, BYTES, j * BYTES) == BYTES);
        }
      } else {
        __atomic_store_n(tail, count + DEPTH, __ATOMIC_RELEASE);
        assert(syscall(SYS_io_uring_enter, fd, DEPTH, 0, 0, NULL, 0) == DEPTH);
        for (unsigned j = 0; j < DEPTH; ++j) {
          assert(cq[(count + j) % p.cq_entries].res == BYTES);
        }
        count += DEPTH;
        __atomic_store_n(head, count, __ATOMIC_RELEASE);
      }
    }
    printf("%s, %u-byte tmpfs reads: %.2f ns/request\n", names[mode], BYTES,
           (nanoseconds() - start) / (double)REQUESTS);
  }
  return 0;
}
