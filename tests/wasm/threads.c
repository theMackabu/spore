#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

static _Thread_local volatile int local;
static atomic_int stage;

static void *worker(void *unused) {
  (void)unused;
  local = 17;
  atomic_store(&stage, 1);
  while (atomic_load(&stage) == 1) {}
  return (void *)(uintptr_t)local;
}

int main(void) {
  struct timespec delay = {.tv_nsec = 30000000};
  if (nanosleep(&delay, NULL)) { return 1; }
  local = 42;
  pthread_t thread;
  if (pthread_create(&thread, NULL, worker, NULL)) { return 2; }
  while (!atomic_load(&stage)) {}
  atomic_store(&stage, 2);
  void *result;
  if (pthread_join(thread, &result) || (uintptr_t)result != 17 || local != 42) { return 3; }
  return 42;
}
