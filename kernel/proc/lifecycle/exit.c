#include "proc/process.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "proc/memory.h"
#include "proc/fd.h"
#include "proc/futex.h"
#include "init/app.h"
#include "lib/kprintf.h"

static void clear_child_tid(struct thread *thread) {
  struct domain *domain = thread->domain;
  if (thread->clear_child_tid != 0) {
    uint32_t zero = 0;
    struct user_address_space *as = cell_domain_as(domain);
    if (as != NULL) {
      (void)vmm_copy_to_user(as, thread->clear_child_tid, &zero, sizeof(zero));
    }
    (void)cell_futex_wake_domain(domain, thread->clear_child_tid, 1, UINT32_MAX);
  }
}

static void mark_domain_exited(struct domain *domain, int status) {
  domain->exit_status = status;
  domain->term_signal = 0;
  domain->zombie = true;
  cell_close_all_fds(domain);
}

static void finish_exit(struct thread *thread, struct trap_frame *frame) {
  struct domain *domain = thread->domain;
  thread->state = THREAD_ZOMBIE;
  thread->running_cpu = -1;
  if (app_mode) { app_finish(APP_EXIT, domain->exit_status); }
  cell_reparent_children(domain, 0, 0);
  cell_wake_parent_of(domain);
  cell_schedule(frame);
}

void cell_exit_thread_current(int status, struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (!thread) { return; }
  struct domain *domain = thread->domain;
  cell_futex_cleanup_robust_list(thread);
  clear_child_tid(thread);
  if (cell_runnable_or_blocked_threads_in_domain(domain) <= 1) {
    mark_domain_exited(domain, status);
    finish_exit(thread, frame);
    return;
  }
  kprintf("[kernel] exit(%d) tid=%d\n", status, thread->tid);
  for (size_t i = 0; i < cell_thread_slot_limit(); ++i) {
    struct thread *sibling = cell_thread_slot(i);
    if (sibling && sibling != thread && sibling->domain == domain &&
        (sibling->state == THREAD_RUNNABLE || sibling->state == THREAD_BLOCKED)) {
      cell_reparent_children(domain, thread->tid, sibling->tid);
      break;
    }
  }
  cell_release_thread(thread);
  cell_schedule(frame);
}

void cell_exit_group_current(int status, struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (!thread) { return; }
  struct domain *domain = thread->domain;
  clear_child_tid(thread);
  mark_domain_exited(domain, status);
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *sibling = cell_thread_slot(i);
    if (sibling != NULL && sibling != thread && sibling->domain == domain) {
      cell_clear_wait(sibling);
      if (sibling->running_cpu >= 0) {
        sibling->state = THREAD_ZOMBIE;
      } else {
        sibling->state = THREAD_UNUSED;
        sibling->running_cpu = -1;
        sibling->domain = NULL;
        if (domain->refcount > 0) { --domain->refcount; }
      }
    }
  }
  finish_exit(thread, frame);
}
