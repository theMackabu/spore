#define _GNU_SOURCE
#ifdef NDEBUG
#error This guest test requires assertions
#endif
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { CPUS = 4, ROUNDS = 128 };
static pthread_barrier_t barrier;
static _Atomic unsigned observed;
static volatile unsigned *page;

static void *reader(void *arg) {
  (void)arg;
  for (unsigned round = 1; round <= ROUNDS; ++round) {
    pthread_barrier_wait(&barrier);
    unsigned cpu;
    assert(syscall(SYS_getcpu, &cpu, NULL, NULL) == 0 && cpu < CPUS);
    atomic_fetch_or(&observed, 1u << cpu);
    assert(*page == round);
    pthread_barrier_wait(&barrier);
  }
  struct timespec start, now;
  assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
  while (atomic_load(&observed) != (1u << CPUS) - 1) {
    unsigned cpu;
    assert(syscall(SYS_getcpu, &cpu, NULL, NULL) == 0 && cpu < CPUS);
    atomic_fetch_or(&observed, 1u << cpu);
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - start.tv_sec < 5);
  }
  return NULL;
}

int main(void) {
  pthread_t threads[CPUS];
  assert(pthread_barrier_init(&barrier, NULL, CPUS + 1) == 0);
  page = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(page != MAP_FAILED);
  for (unsigned i = 0; i < CPUS; ++i) {
    assert(pthread_create(&threads[i], NULL, reader, NULL) == 0);
  }
  for (unsigned round = 1; round <= ROUNDS; ++round) {
    assert(mmap((void *)page, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == page);
    *page = round;
    pthread_barrier_wait(&barrier);
    pthread_barrier_wait(&barrier);
  }
  for (unsigned i = 0; i < CPUS; ++i) {
    assert(pthread_join(threads[i], NULL) == 0);
  }
  assert(atomic_load(&observed) == (1u << CPUS) - 1);
  return 0;
}
