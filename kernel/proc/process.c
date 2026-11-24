#include "init/app.h"
#include "proc/process.h"

#include "arch/aarch64/regs.h"
#include "arch/aarch64/smp.h"
#include "lib/kprintf.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/futex.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/thread.h"

enum {
  EAGAIN = 11,
  EFAULT = 14,
  ENOMEM = 12,
  CLONE_SETTLS = 0x00080000,
  CLONE_PARENT_SETTID = 0x00100000,
  CLONE_CHILD_CLEARTID = 0x00200000,
  CLONE_CHILD_SETTID = 0x01000000,
};

static struct domain *current_domain(void) {
  return cell_current_domain_internal();
}

void cell_exit_thread_current(int status, struct trap_frame *frame) {
  if (cell_current_thread_internal() == NULL) { return; }
  struct domain *domain = cell_current_thread_internal()->domain;
  cell_futex_cleanup_robust_list(cell_current_thread_internal());
  if (cell_current_thread_internal()->clear_child_tid != 0) {
    uint32_t zero = 0;
    struct user_address_space *as = cell_domain_as(domain);
    if (as != NULL) {
      (void)vmm_copy_to_user(as, cell_current_thread_internal()->clear_child_tid, &zero, sizeof(zero));
    }
    (void)cell_futex_wake_domain(domain, cell_current_thread_internal()->clear_child_tid, 1, UINT32_MAX);
  }
  if (cell_runnable_or_blocked_threads_in_domain(domain) <= 1) {
    domain->exit_status = status;
    domain->term_signal = 0;
    domain->zombie = true;
    cell_close_all_fds(domain);
    cell_current_thread_internal()->state = THREAD_ZOMBIE;
    cell_current_thread_internal()->running_cpu = -1;
    if (app_mode) { app_finish(APP_EXIT, status); }
    cell_wake_parent_of(domain);
    cell_schedule(frame);
    return;
  }
  kprintf("[kernel] exit(%d) tid=%d\n", status, cell_current_thread_internal()->tid);
  cell_release_thread(cell_current_thread_internal());
  cell_schedule(frame);
}

void cell_exit_group_current(int status, struct trap_frame *frame) {
  if (cell_current_thread_internal() == NULL) { return; }
  struct domain *domain = cell_current_thread_internal()->domain;
  if (cell_current_thread_internal()->clear_child_tid != 0) {
    uint32_t zero = 0;
    struct user_address_space *as = cell_domain_as(domain);
    if (as != NULL) {
      (void)vmm_copy_to_user(as, cell_current_thread_internal()->clear_child_tid, &zero, sizeof(zero));
    }
    (void)cell_futex_wake_domain(domain, cell_current_thread_internal()->clear_child_tid, 1, UINT32_MAX);
  }
  domain->exit_status = status;
  domain->term_signal = 0;
  domain->zombie = true;
  cell_close_all_fds(domain);
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread != cell_current_thread_internal() && thread->domain == domain) {
      cell_clear_wait(thread);
      if (thread->running_cpu >= 0) {
        thread->state = THREAD_ZOMBIE;
      } else {
        thread->state = THREAD_UNUSED;
        thread->running_cpu = -1;
        thread->domain = NULL;
        if (domain->refcount > 0) { --domain->refcount; }
      }
    }
  }
  cell_current_thread_internal()->state = THREAD_ZOMBIE;
  cell_current_thread_internal()->running_cpu = -1;
  if (app_mode) { app_finish(APP_EXIT, status); }
  cell_wake_parent_of(domain);
  cell_schedule(frame);
}

static void inherit_fork_context(struct thread *child, const struct thread *parent) {
  child->state = THREAD_RUNNABLE;
  child->tf = parent->tf;
  child->context = parent->context;
  child->signal_mask = parent->signal_mask;
  child->pending_signals = 0;
  child->sigaltstack_sp = parent->sigaltstack_sp;
  child->sigaltstack_size = parent->sigaltstack_size;
  child->sigaltstack_flags = parent->sigaltstack_flags;
  trap_set_result(&child->tf, 0);
}

int cell_fork_current(struct trap_frame *frame) {
  struct domain *parent = current_domain();
  if (parent == NULL || parent->mm == NULL) { return -ENOMEM; }
  struct thread *child_thread = cell_alloc_process();
  if (child_thread == NULL) { return -EAGAIN; }
  struct domain *child_domain = child_thread->domain;
  cell_save_current(frame);
  struct process_mm *child_mm = cell_mm_clone_cow(parent->mm);
  if (child_mm == NULL || !cell_domain_set_mm(child_domain, child_mm)) {
    if (child_mm != NULL) { cell_mm_release(child_mm); }
    cell_destroy_domain(child_domain);
    return -ENOMEM;
  }
  cell_copy_domain_metadata(child_domain, parent);
  if (!cell_copy_fd_table(child_domain, parent)) { cell_destroy_domain(child_domain); return -12; }
  child_domain->parent_id = parent->id;
  inherit_fork_context(child_thread, cell_current_thread_internal());
  trap_set_result(&cell_current_thread_internal()->tf, (uint64_t)child_domain->id);
  copy_cstr(child_domain->name, sizeof(child_domain->name), parent->name);
  smp_notify();
  return child_domain->id;
}

int cell_vfork_current(struct trap_frame *frame, uint64_t newsp, uint64_t flags, uint64_t parent_tid, uint64_t tls,
                       uint64_t child_tid) {
  struct domain *parent = current_domain();
  if (parent == NULL || parent->mm == NULL) { return -ENOMEM; }
  struct thread *child_thread = cell_alloc_process();
  if (child_thread == NULL) { return -EAGAIN; }
  struct domain *child = child_thread->domain;
  cell_save_current(frame);
  if (!cell_domain_set_mm(child, cell_mm_retain(parent->mm))) {
    cell_destroy_domain(child);
    return -ENOMEM;
  }
  cell_copy_domain_metadata(child, parent);
  if (!cell_copy_fd_table(child, parent)) { cell_destroy_domain(child); return -12; }
  child->parent_id = parent->id;
  inherit_fork_context(child_thread, cell_current_thread_internal());
  if (newsp != 0) { trap_set_stack(&child_thread->tf, newsp); }
  if ((flags & CLONE_SETTLS) != 0) { child_thread->context.thread_pointer = tls; }
  child_thread->clear_child_tid = ((flags & CLONE_CHILD_CLEARTID) != 0) ? child_tid : 0;
  if ((flags & CLONE_PARENT_SETTID) != 0 && parent_tid != 0) {
    uint32_t pid = (uint32_t)child->id;
    struct user_address_space *as = cell_domain_as(parent);
    if (as == NULL || !vmm_copy_to_user(as, parent_tid, &pid, sizeof(pid))) {
      cell_release_thread(child_thread);
      cell_destroy_domain(child);
      return -EFAULT;
    }
  }
  if ((flags & CLONE_CHILD_SETTID) != 0 && child_tid != 0) {
    uint32_t pid = (uint32_t)child->id;
    struct user_address_space *as = cell_domain_as(child);
    if (as == NULL || !vmm_copy_to_user(as, child_tid, &pid, sizeof(pid))) {
      cell_release_thread(child_thread);
      cell_destroy_domain(child);
      return -EFAULT;
    }
  }
  trap_set_result(&cell_current_thread_internal()->tf, (uint64_t)child->id);
  trap_set_result(frame, (uint64_t)child->id);
  copy_cstr(child->name, sizeof(child->name), parent->name);
  cell_current_thread_internal()->running_cpu = -1;
  cell_current_thread_internal()->wait.reason = WAIT_VFORK;
  cell_current_thread_internal()->wait.target = child->id;
  cell_mark_blocked(cell_current_thread_internal());
  smp_notify();
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_clone_thread_current(struct trap_frame *frame, uint64_t flags, uint64_t newsp, uint64_t parent_tid,
                              uint64_t tls, uint64_t child_tid) {
  struct domain *domain = current_domain();
  if (domain == NULL || cell_current_thread_internal() == NULL || newsp == 0) { return -22; }
  struct thread *thread = cell_alloc_thread(domain, false);
  if (thread == NULL) { return -EAGAIN; }
  cell_save_current(frame);
  thread->state = THREAD_RUNNABLE;
  thread->tf = cell_current_thread_internal()->tf;
  thread->context = cell_current_thread_internal()->context;
  thread->signal_mask = cell_current_thread_internal()->signal_mask;
  thread->pending_signals = 0;
  thread->sigaltstack_flags = 2;
  trap_set_result(&thread->tf, 0);
  trap_set_stack(&thread->tf, newsp);
  if ((flags & 0x00080000ull) != 0) { thread->context.thread_pointer = tls; }
  thread->clear_child_tid = ((flags & 0x00200000ull) != 0) ? child_tid : 0;
  if ((flags & 0x00100000ull) != 0 && parent_tid != 0) {
    uint32_t tid = (uint32_t)thread->tid;
    struct user_address_space *as = cell_domain_as(domain);
    if (as != NULL) { (void)vmm_copy_to_user(as, parent_tid, &tid, sizeof(tid)); }
  }
  if ((flags & 0x01000000ull) != 0 && child_tid != 0) {
    uint32_t tid = (uint32_t)thread->tid;
    struct user_address_space *as = cell_domain_as(domain);
    if (as != NULL) { (void)vmm_copy_to_user(as, child_tid, &tid, sizeof(tid)); }
  }
  trap_set_result(&cell_current_thread_internal()->tf, (uint64_t)thread->tid);
  smp_notify();
  return thread->tid;
}

int cell_set_tid_address_current(uint64_t clear_child_tid) {
  if (cell_current_thread_internal() == NULL) { return 0; }
  cell_current_thread_internal()->clear_child_tid = clear_child_tid;
  return cell_current_thread_internal()->tid;
}

bool cell_exec_replace(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp,
                       struct trap_frame *frame, const char *path, const char *const argv[], uint64_t argc) {
  struct domain *domain = current_domain();
  if (domain == NULL || cell_current_thread_internal() == NULL) { return false; }

  struct process_mm *new_mm = cell_mm_from_owned(as, vmas);
  if (new_mm == NULL) { return false; }
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread != cell_current_thread_internal() && thread->domain == domain) {
      cell_clear_wait(thread);
      thread->state = THREAD_UNUSED;
      thread->domain = NULL;
      if (domain->refcount > 0) { --domain->refcount; }
    }
  }
  cell_close_exec_fds(domain);
  cell_domain_set_mm(domain, new_mm);
  kmemset(domain->signal_actions, 0, sizeof(domain->signal_actions));
  cell_set_domain_identity(domain, path, argv, argc);
  vmm_install_user(cell_domain_as(domain));

  aarch64_init_user_frame(frame, entry, sp, cell_domain_as(domain)->freebsd_abi);
  struct thread *thread = cell_current_thread_internal();
  thread->tid = domain->id;
  thread->clear_child_tid = 0;
  thread->robust_list = 0;
  thread->sigaltstack_flags = 2;
  thread->sigaltstack_sp = 0;
  thread->sigaltstack_size = 0;
  thread->tf = *frame;
  struct aarch64_thread_context *context = &thread->context;
  kmemset(context, 0, sizeof(*context));
  aarch64_restore_context(context);
  cell_wake_vfork_parent_of(domain->id);
  return true;
}

int cell_set_budget(int domain_id, uint64_t ticks) {
  struct domain *domain = domain_id == 0 ? current_domain() : cell_find_domain(domain_id);
  if (domain == NULL) { return -3; }
  domain->budget.max_ticks = ticks;
  domain->budget.remaining_ticks = ticks;
  if (ticks != 0) { kprintf("[spore] domain %d CPU budget set to %u ticks\n", domain->id, (unsigned)ticks); }
  return 0;
}
