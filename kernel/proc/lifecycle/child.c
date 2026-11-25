#include "proc/pid_namespace.h"
#include "proc/process.h"
#include "abi/linux/shared/usage.h"
#include "arch/smp.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/user_namespace.h"
#include "proc/thread.h"

enum { ECHILD = 10, EFAULT = 14, WNOHANG = 1 };

static bool domain_has_running_thread(const struct domain *domain) {
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain && thread->running_cpu >= 0) { return true; }
  }
  return false;
}

void cell_reap_namespace_orphan(struct domain *child) {
  if (child->used && child->zombie && !child->parent_id && pidns_active(child) &&
      child != cell_current_domain_internal() && !domain_has_running_thread(child)) { cell_destroy_domain(child); }
}

static bool matches(const struct domain *child, const struct domain *parent, int pid) {
  if (!child->used || child->parent_id != parent->id) { return false; }
  if (pid > 0) { return pidns_local(pidns_active(parent), child->id) == pid; }
  if (pid == -1) { return true; }
  return pid ? pidns_local(pidns_active(parent), child->pgrp_id) == -(int64_t)pid : child->pgrp_id == parent->pgrp_id;
}

static int reap(struct domain *parent, struct domain *child, uint64_t status_addr, uint64_t usage_addr) {
  int status = child->term_signal ? child->term_signal : (child->exit_status & 255) << 8;
  if (status_addr && (!cell_domain_ensure_user_range(parent, status_addr, sizeof(status), VMM_ACCESS_WRITE) ||
                      !vmm_copy_to_user(cell_domain_as(parent), status_addr, &status, sizeof(status)))) { return -EFAULT; }
  struct resource_usage usage = cell_process_usage(child);
  cell_merge_usage(&usage, &child->children_usage);
  if (usage_addr && linux_write_rusage(parent, usage_addr, &usage) < 0) { return -EFAULT; }
  cell_merge_usage(&parent->children_usage, &usage);
  int id = pidns_local(pidns_active(parent), child->id);
  child->parent_id = 0;
  if (child != cell_current_domain_internal() && !domain_has_running_thread(child)) { cell_destroy_domain(child); }
  return id;
}

static int wait_result(struct domain *parent, int pid, uint64_t status_addr, uint64_t usage_addr) {
  int result = -ECHILD;
  for (size_t i = 0, limit = cell_domain_slot_limit(); i < limit; ++i) {
    struct domain *child = cell_domain_slot(i);
    if (!matches(child, parent, pid)) { continue; }
    if (child->zombie) { return reap(parent, child, status_addr, usage_addr); }
    result = 0;
  }
  return result;
}

void cell_wake_parent_of(struct domain *child) {
  cell_wake_vfork_parent_of(child->id);
  if (!child->parent_id) { cell_reap_namespace_orphan(child); return; }
  int parent_id = child->parent_id;
  int signal = child->exit_signal;
  struct domain *parent = cell_find_domain(parent_id);
  struct siginfo_fields64 info = {.si_signo = signal, .si_code = child->term_signal ? 2 : 1,
                                  .sender = {pidns_local(pidns_active(parent), child->id), child->uid},
                                  .child_status = child->term_signal ? child->term_signal : child->exit_status & 255};
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_CHILD, &cursor))) {
    if (thread->domain->id != parent_id || thread->wait.child_servicing) { continue; }
    thread->wait.child_servicing = true;
    int result = wait_result(thread->domain, thread->wait.target, thread->wait.child_status, thread->wait.child_usage);
    thread->wait.child_servicing = false;
    if (result) { cell_finish_wait(thread, result); smp_notify(); }
  }
  if (signal && parent && !parent->zombie) {
    info.sender.uid = userns_visible(parent->user_ns, false, info.sender.uid);
    if (signal == 17) { cell_signal_child_exit(parent, info); }
    else { cell_signal_domain(parent, signal, NULL); }
  }
}

int cell_wait4_options(int pid, uint64_t status_addr, int options, uint64_t usage_addr, struct trap_frame *frame) {
  struct domain *parent = cell_current_domain_internal();
  if (!parent) { return -ECHILD; }
  int result = wait_result(parent, pid, status_addr, usage_addr);
  if (result || (options & WNOHANG)) { return result; }
  return cell_block_current(frame, (struct thread_wait){.reason = WAIT_CHILD, .target = pid,
                                                        .child_status = status_addr, .child_usage = usage_addr});
}
