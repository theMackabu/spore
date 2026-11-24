#include "proc/info.h"
#include "lib/kstr.h"
#include "proc/domain.h"
#include "proc/memory.h"

enum thread_state cell_domain_state(const struct domain *domain) {
  if (domain == NULL) { return THREAD_UNUSED; }
  if (domain->zombie) { return THREAD_ZOMBIE; }
  bool blocked = false;
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->domain != domain || thread->state == THREAD_UNUSED) { continue; }
    if (thread->state == THREAD_RUNNABLE) { return THREAD_RUNNABLE; }
    if (thread->state == THREAD_BLOCKED) { blocked = true; }
  }
  return blocked ? THREAD_BLOCKED : THREAD_UNUSED;
}

static void fill_proc_info(const struct domain *domain, struct proc_info *out) {
  struct cell_memory_accounting mem = {0};
  (void)cell_memory_accounting(domain, &mem);
  struct thread *thread = cell_thread_for_domain(domain);
  enum thread_state state = cell_domain_state(domain);
  struct proc_info info = {
    .pid = (uint32_t)domain->id,
    .tid = thread == NULL ? 0 : (uint32_t)thread->tid,
    .ppid = (uint32_t)domain->parent_id,
    .state = state == THREAD_UNUSED ? THREAD_RUNNABLE : state,
    .wait_reason = 0,
    .resident_pages = mem.resident_pages,
    .virtual_pages = mem.virtual_pages,
    .minor_faults = mem.minor_faults,
    .major_faults = mem.major_faults,
    .cpu_ticks = domain->cpu_ticks,
    .start_ticks = domain->start_ticks,
    .remaining_ticks = domain->budget.remaining_ticks,
    .max_ticks = domain->budget.max_ticks,
  };
  copy_cstr(info.name, sizeof(info.name), domain->name);
  copy_cstr(info.exec_path, sizeof(info.exec_path), domain->exec_path);
  copy_cstr(info.argv0, sizeof(info.argv0), domain->argv0);
  copy_cstr(info.cmdline, sizeof(info.cmdline), domain->cmdline);
  copy_cstr(info.cwd, sizeof(info.cwd), domain->cwd);
  *out = info;
}

size_t cell_proc_info(struct proc_info *out, size_t max) {
  size_t count = 0;
  for (size_t i = 0, limit = cell_domain_slot_limit(); i < limit; ++i) {
    const struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used) { continue; }
    if (count < max && out != NULL) { fill_proc_info(domain, &out[count]); }
    ++count;
  }
  return count;
}
