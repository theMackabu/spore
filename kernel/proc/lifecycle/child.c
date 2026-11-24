#include "proc/process.h"
#include "abi/linux/shared/usage.h"
#include "arch/smp.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/thread.h"

enum { ECHILD = 10, EFAULT = 14, WNOHANG = 1 };

static bool domain_has_running_thread(const struct domain *domain) {
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain && thread->running_cpu >= 0) { return true; }
  }
  return false;
}

static bool matches(const struct domain *child, const struct domain *parent, int pid) {
  if (!child->used || child->parent_id != parent->id) { return false; }
  if (pid > 0) { return child->id == pid; }
  if (pid == -1) { return true; }
  return child->pgrp_id == (pid ? -(int64_t)pid : parent->pgrp_id);
}

static int reap(struct domain *parent, struct domain *child, uint64_t status_addr, uint64_t usage_addr) {
  int status = child->term_signal ? child->term_signal : (child->exit_status & 255) << 8;
  if (status_addr && (!cell_domain_ensure_user_range(parent, status_addr, sizeof(status), VMM_ACCESS_WRITE) ||
                      !vmm_copy_to_user(cell_domain_as(parent), status_addr, &status, sizeof(status)))) { return -EFAULT; }
  struct resource_usage usage = cell_process_usage(child);
  cell_merge_usage(&usage, &child->children_usage);
  if (usage_addr && linux_write_rusage(parent, usage_addr, &usage) < 0) { return -EFAULT; }
  cell_merge_usage(&parent->children_usage, &usage);
  int id = child->id;
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
  int parent_id = child->parent_id;
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_CHILD, &cursor))) {
    if (thread->domain->id != parent_id || thread->wait.child_servicing) { continue; }
    thread->wait.child_servicing = true;
    int result = wait_result(thread->domain, thread->wait.target, thread->wait.child_status, thread->wait.child_usage);
    thread->wait.child_servicing = false;
    if (result) { cell_finish_wait(thread, result); smp_notify(); }
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
