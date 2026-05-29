#include "proc/process.h"

#include "kprintf.h"
#include "kstr.h"
#include "mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/futex.h"
#include "proc/thread.h"

extern void arch_fp_restore(const struct fp_state *state);

enum {
  ECHILD = 10,
  WNOHANG = 1,
  CELL_O_CLOEXEC = 02000000,
};

static struct domain *current_domain(void) {
  return cell_current_domain_internal();
}

void cell_wake_parent_of(struct domain *child) {
  cell_wake_vfork_parent_of(child->id);
  struct domain *parent_domain = cell_find_domain(child->parent_id);
  struct thread *parent = parent_domain == NULL ? NULL : cell_thread_for_domain(parent_domain);
  if (parent != NULL && parent->state == THREAD_BLOCKED && parent->wait_reason == WAIT_CHILD &&
      (parent->wait_target < 0 || parent->wait_target == child->id)) {
    bool child_is_current = child == current_domain();
    int status = child->exit_status << 8;
    if (child->term_signal != 0) { status = child->term_signal; }
    uint64_t status_addr = parent->tf.x[1];
    struct user_address_space *parent_as = cell_domain_as(parent_domain);
    if (status_addr != 0 && parent_as != NULL) { (void)vmm_copy_to_user(parent_as, status_addr, &status, sizeof(status)); }
    parent->tf.x[0] = (uint64_t)child->id;
    struct thread *child_thread = cell_thread_for_domain(child);
    if (child_thread != NULL) { child_thread->state = child_is_current ? THREAD_ZOMBIE : THREAD_UNUSED; }
    if (child_is_current) {
      child->parent_id = 0;
    } else {
      cell_destroy_domain(child);
    }
    parent->state = THREAD_RUNNABLE;
    parent->wait_reason = WAIT_NONE;
    parent->wait_target = -1;
  }
}

void cell_exit_thread_current(int status, struct trap_frame *frame) {
  if (cell_current_thread_internal() == NULL) { return; }
  struct domain *domain = cell_current_thread_internal()->domain;
  cell_futex_cleanup_robust_list(cell_current_thread_internal());
  if (cell_current_thread_internal()->clear_child_tid != 0) {
    uint32_t zero = 0;
    struct user_address_space *as = cell_domain_as(domain);
    if (as != NULL) { (void)vmm_copy_to_user(as, cell_current_thread_internal()->clear_child_tid, &zero, sizeof(zero)); }
    (void)cell_futex_wake_domain(domain, cell_current_thread_internal()->clear_child_tid, 1);
  }
  if (cell_runnable_or_blocked_threads_in_domain(domain) <= 1) {
    domain->exit_status = status;
    domain->term_signal = 0;
    domain->zombie = true;
    cell_close_all_fds(domain);
    cell_current_thread_internal()->state = THREAD_ZOMBIE;
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
  domain->exit_status = status;
  domain->term_signal = 0;
  domain->zombie = true;
  cell_close_all_fds(domain);
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread != cell_current_thread_internal() && thread->domain == domain) {
      thread->state = THREAD_UNUSED;
      thread->domain = NULL;
      if (domain->refcount > 0) { --domain->refcount; }
    }
  }
  cell_current_thread_internal()->state = THREAD_ZOMBIE;
  cell_wake_parent_of(domain);
  cell_schedule(frame);
}

int cell_fork_current(struct trap_frame *frame) {
  struct domain *parent = current_domain();
  struct domain *child_domain = cell_alloc_domain();
  if (parent == NULL || parent->mm == NULL || child_domain == NULL) { return -12; }
  struct thread *child_thread = cell_alloc_thread(child_domain);
  if (child_thread == NULL) {
    child_domain->used = false;
    return -12;
  }
  cell_save_current(frame);
  struct process_mm *child_mm = cell_mm_clone_cow(parent->mm);
  if (child_mm == NULL || !cell_domain_set_mm(child_domain, child_mm)) {
    if (child_mm != NULL) { cell_mm_release(child_mm); }
    child_thread->state = THREAD_UNUSED;
    child_domain->used = false;
    return -12;
  }
  cell_copy_domain_metadata(child_domain, parent);
  cell_copy_fd_table(child_domain, parent);
  child_domain->parent_id = parent->id;
  child_thread->state = THREAD_RUNNABLE;
  child_thread->tf = cell_current_thread_internal()->tf;
  child_thread->fp = cell_current_thread_internal()->fp;
  child_thread->tf.x[0] = 0;
  child_thread->tpidr_el0 = cell_current_thread_internal()->tpidr_el0;
  cell_current_thread_internal()->tf.x[0] = (uint64_t)child_domain->id;
  copy_cstr(child_domain->name, sizeof(child_domain->name), parent->name);
  return child_domain->id;
}

int cell_vfork_current(struct trap_frame *frame, uint64_t newsp) {
  struct domain *parent = current_domain();
  if (parent == NULL || parent->mm == NULL) { return -12; }
  struct domain *child = cell_alloc_domain();
  if (child == NULL) { return -12; }
  struct thread *child_thread = cell_alloc_thread(child);
  if (child_thread == NULL) {
    child->used = false;
    return -12;
  }
  cell_save_current(frame);
  if (!cell_domain_set_mm(child, cell_mm_retain(parent->mm))) {
    child_thread->state = THREAD_UNUSED;
    child->used = false;
    return -12;
  }
  cell_copy_domain_metadata(child, parent);
  cell_copy_fd_table(child, parent);
  child->parent_id = parent->id;
  child_thread->state = THREAD_RUNNABLE;
  child_thread->tf = cell_current_thread_internal()->tf;
  child_thread->fp = cell_current_thread_internal()->fp;
  child_thread->tf.x[0] = 0;
  if (newsp != 0 && child_thread != NULL) { child_thread->tf.sp_el0 = newsp; }
  child_thread->tpidr_el0 = cell_current_thread_internal()->tpidr_el0;
  cell_current_thread_internal()->tf.x[0] = (uint64_t)child->id;
  copy_cstr(child->name, sizeof(child->name), parent->name);
  cell_current_thread_internal()->state = THREAD_BLOCKED;
  cell_current_thread_internal()->wait_reason = WAIT_VFORK;
  cell_current_thread_internal()->wait_target = child->id;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_clone_thread_current(struct trap_frame *frame, uint64_t flags, uint64_t newsp, uint64_t parent_tid,
                              uint64_t tls, uint64_t child_tid) {
  struct domain *domain = current_domain();
  if (domain == NULL || cell_current_thread_internal() == NULL || newsp == 0) { return -22; }
  struct thread *thread = cell_alloc_thread(domain);
  if (thread == NULL) { return -12; }
  cell_save_current(frame);
  thread->state = THREAD_RUNNABLE;
  thread->tf = cell_current_thread_internal()->tf;
  thread->fp = cell_current_thread_internal()->fp;
  thread->tf.x[0] = 0;
  thread->tf.sp_el0 = newsp;
  thread->tpidr_el0 = ((flags & 0x00080000ull) != 0) ? tls : cell_current_thread_internal()->tpidr_el0;
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
  cell_current_thread_internal()->tf.x[0] = (uint64_t)thread->tid;
  return thread->tid;
}

int cell_set_tid_address_current(uint64_t clear_child_tid) {
  if (cell_current_thread_internal() == NULL) { return 0; }
  cell_current_thread_internal()->clear_child_tid = clear_child_tid;
  return cell_current_thread_internal()->tid;
}

static struct domain *find_waitable_child(int parent_id, int pid) {
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain != NULL && domain->used && domain->zombie && domain->parent_id == parent_id &&
        (pid <= 0 || domain->id == pid)) {
      return domain;
    }
  }
  return NULL;
}

static bool has_child(int parent_id, int pid) {
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain != NULL && domain->used && domain->parent_id == parent_id && (pid <= 0 || domain->id == pid)) {
      return true;
    }
  }
  return false;
}

int cell_wait4_options(int pid, uint64_t status_addr, int options, struct trap_frame *frame) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return -ECHILD; }
  struct domain *child = find_waitable_child(domain->id, pid);
  if (child == NULL) {
    if (!has_child(domain->id, pid)) { return -ECHILD; }
    if ((options & WNOHANG) != 0) { return 0; }
    cell_save_current(frame);
    cell_current_thread_internal()->state = THREAD_BLOCKED;
    cell_current_thread_internal()->wait_reason = WAIT_CHILD;
    cell_current_thread_internal()->wait_target = pid;
    cell_schedule(frame);
    return CELL_SWITCHED;
  }

  int status = child->exit_status << 8;
  if (child->term_signal != 0) { status = child->term_signal; }
  struct user_address_space *as = cell_domain_as(domain);
  if (status_addr != 0 && (as == NULL || !vmm_copy_to_user(as, status_addr, &status, sizeof(status)))) { return -14; }
  int child_id = child->id;
  struct thread *child_thread = cell_thread_for_domain(child);
  if (child_thread != NULL) { child_thread->state = THREAD_UNUSED; }
  cell_destroy_domain(child);
  cell_current_thread_internal()->wait_target = -1;
  cell_current_thread_internal()->wait_reason = WAIT_NONE;
  return child_id;
}

int cell_wait4(int pid, uint64_t status_addr, struct trap_frame *frame) {
  return cell_wait4_options(pid, status_addr, 0, frame);
}

bool cell_exec_replace(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp,
                       struct trap_frame *frame, const char *path, const char *const argv[], uint64_t argc) {
  struct domain *domain = current_domain();
  if (domain == NULL || cell_current_thread_internal() == NULL) { return false; }

  struct process_mm *new_mm = cell_mm_from_owned(as, vmas);
  if (new_mm == NULL) { return false; }
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread != cell_current_thread_internal() && thread->domain == domain) {
      thread->state = THREAD_UNUSED;
      thread->domain = NULL;
      if (domain->refcount > 0) { --domain->refcount; }
    }
  }
  for (size_t i = 0; i < MAX_FDS; ++i) {
    if (domain->fds[i] != NULL && domain->fd_flags[i] != 0) {
      cell_release_open_file(domain->fds[i]);
      domain->fds[i] = NULL;
      domain->fd_flags[i] = 0;
    }
  }
  cell_domain_set_mm(domain, new_mm);
  kmemset(domain->signal_actions, 0, sizeof(domain->signal_actions));
  cell_set_domain_identity(domain, path, argv, argc);
  vmm_install_user(cell_domain_as(domain));

  kmemset(frame, 0, sizeof(*frame));
  frame->elr_el1 = entry;
  frame->sp_el0 = sp;
  frame->spsr_el1 = 0x340;
  cell_current_thread_internal()->tf = *frame;
  kmemset(&cell_current_thread_internal()->fp, 0, sizeof(cell_current_thread_internal()->fp));
  arch_fp_restore(&cell_current_thread_internal()->fp);
  cell_current_thread_internal()->tpidr_el0 = 0;
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(0ull));
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
