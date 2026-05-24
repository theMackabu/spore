#include "cell.h"

#include <assert.h>
#include <string.h>

static struct thread *alloc_model_thread(struct thread *threads, struct domain *domain, int tid) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].state == THREAD_UNUSED) {
      memset(&threads[i], 0, sizeof(threads[i]));
      threads[i].tid = tid;
      threads[i].domain = domain;
      threads[i].state = THREAD_RUNNABLE;
      ++domain->refcount;
      return &threads[i];
    }
  }
  return 0;
}

static void release_model_thread(struct thread *thread) {
  assert(thread != 0);
  assert(thread->domain != 0);
  assert(thread->domain->refcount > 0);
  --thread->domain->refcount;
  thread->domain = 0;
  thread->state = THREAD_UNUSED;
}

static size_t owned_threads(const struct thread *threads, const struct domain *domain) {
  size_t count = 0;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].domain == domain && threads[i].state != THREAD_UNUSED) { ++count; }
  }
  return count;
}

int main(void) {
  struct domain domain = {.id = 7, .used = true};
  struct thread threads[MAX_THREADS];
  memset(threads, 0, sizeof(threads));

  struct thread *main_thread = alloc_model_thread(threads, &domain, 100);
  struct thread *worker_a = alloc_model_thread(threads, &domain, 101);
  struct thread *worker_b = alloc_model_thread(threads, &domain, 102);
  assert(main_thread != 0 && worker_a != 0 && worker_b != 0);
  assert(domain.refcount == 3);
  assert(owned_threads(threads, &domain) == 3);
  assert(main_thread->tid != worker_a->tid);
  assert(worker_a->tid != worker_b->tid);

  worker_a->clear_child_tid = 0x4000;
  worker_a->robust_list = 0x8000;
  assert(worker_a->clear_child_tid == 0x4000);
  assert(worker_a->robust_list == 0x8000);

  release_model_thread(worker_a);
  assert(domain.refcount == 2);
  assert(owned_threads(threads, &domain) == 2);

  release_model_thread(worker_b);
  release_model_thread(main_thread);
  assert(domain.refcount == 0);
  assert(owned_threads(threads, &domain) == 0);
  return 0;
}
