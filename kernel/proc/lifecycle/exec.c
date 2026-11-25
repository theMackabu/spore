#include "proc/pid_namespace.h"
#include "proc/process.h"
#include "proc/domain.h"
#include "proc/thread.h"
#include "proc/memory.h"
#include "proc/fd.h"
#include "lib/mem.h"

bool cell_exec_replace(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp,
                       struct trap_frame *frame, const char *path, const char *const argv[], uint64_t argc) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || cell_current_thread_internal() == NULL) { return false; }

  struct process_mm *new_mm = cell_mm_from_owned(as, vmas);
  if (new_mm == NULL) { return false; }
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread != cell_current_thread_internal() && thread->domain == domain) {
      cell_reparent_children(domain, thread->tid, cell_current_tid());
      cell_clear_wait(thread);
      pidns_drop_id(thread->pid_entry);
      thread->pid_entry = NULL;
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

  arch_init_user_frame(frame, entry, sp, cell_domain_as(domain)->freebsd_abi);
  struct thread *thread = cell_current_thread_internal();
  if (thread->tid != domain->id) {
    for (size_t i = 0; i < cell_domain_slot_limit(); ++i) {
      struct domain *child = cell_domain_slot(i);
      if (child->used && child->parent_id == domain->id) { child->parent_tid = domain->id; }
    }
    pidns_drop_id(thread->pid_entry);
    thread->pid_entry = NULL;
    thread->tid = domain->id;
  }
  thread->clear_child_tid = 0;
  thread->robust_list = 0;
  thread->sigaltstack_flags = 2;
  thread->sigaltstack_sp = 0;
  thread->sigaltstack_size = 0;
  thread->tf = *frame;
  arch_reset_context(&thread->context);
  cell_wake_vfork_parent_of(domain->id);
  return true;
}
