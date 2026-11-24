#include "proc/process.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "proc/memory.h"
#include "proc/fd.h"
#include "arch/smp.h"
#include "lib/kstr.h"

enum {
  EAGAIN = 11,
  EFAULT = 14,
  ENOMEM = 12,
  CLONE_SETTLS = 0x00080000,
  CLONE_PARENT_SETTID = 0x00100000,
  CLONE_CHILD_CLEARTID = 0x00200000,
  CLONE_CHILD_SETTID = 0x01000000,
};

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
  struct domain *parent = cell_current_domain_internal();
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
  struct domain *parent = cell_current_domain_internal();
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
  if ((flags & CLONE_SETTLS) != 0) { child_thread->context.thread_pointer = tls;
#ifdef __riscv
    child_thread->tf.tp = tls;
#endif
  }
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
  struct domain *domain = cell_current_domain_internal();
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
  if ((flags & CLONE_SETTLS) != 0) { thread->context.thread_pointer = tls;
#ifdef __riscv
    thread->tf.tp = tls;
#endif
  }
  thread->clear_child_tid = ((flags & CLONE_CHILD_CLEARTID) != 0) ? child_tid : 0;
  if ((flags & CLONE_PARENT_SETTID) != 0 && parent_tid != 0) {
    uint32_t tid = (uint32_t)thread->tid;
    struct user_address_space *as = cell_domain_as(domain);
    if (as != NULL) { (void)vmm_copy_to_user(as, parent_tid, &tid, sizeof(tid)); }
  }
  if ((flags & CLONE_CHILD_SETTID) != 0 && child_tid != 0) {
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
