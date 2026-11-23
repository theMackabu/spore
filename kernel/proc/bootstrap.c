#include "proc/bootstrap.h"
#include "accounting_internal.h"
#include "arch/aarch64/platform.h"
#include "arch/aarch64/regs.h"
#include "exec/stack.h"
#include "lib/kprintf.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/pipe.h"
#include "proc/snapshot.h"
#include "proc/thread.h"
#include "tty/tty.h"

static size_t clamp_size(size_t value, size_t min, size_t max) {
  if (value < min) { return min; }
  if (value > max) { return max; }
  return value;
}

static void derive_table_sizes(size_t *domains, size_t *threads, size_t *mms, size_t *open_files) {
  uint64_t pages = pmm_total_pages();
  size_t domain_count = clamp_size((size_t)(pages / 32768), 64, MAX_DOMAINS);
  size_t thread_count = clamp_size(domain_count * 2, 128, MAX_THREADS);
  size_t mm_count = domain_count + MAX_SNAPSHOTS;
  if (mm_count > MAX_DOMAINS + MAX_SNAPSHOTS) { mm_count = MAX_DOMAINS + MAX_SNAPSHOTS; }
  size_t open_file_count = clamp_size(domain_count * 4, 512, MAX_OPEN_FILES);
  *domains = domain_count;
  *threads = thread_count;
  *mms = mm_count;
  *open_files = open_file_count;
}

static void halt_table_init_failure(const char *name) {
  kprintf("[spore] failed to allocate %s table\n", name);
  aarch64_halt();
}

void cell_system_init(uint64_t hhdm_offset) {
  vma_set_hhdm_offset(hhdm_offset);
  size_t domain_count = 0;
  size_t thread_count = 0;
  size_t mm_count = 0;
  size_t open_file_count = 0;
  derive_table_sizes(&domain_count, &thread_count, &mm_count, &open_file_count);
  if (!cell_mm_reset(mm_count)) { halt_table_init_failure("mm"); }
  if (!cell_domain_reset(domain_count)) { halt_table_init_failure("domain"); }
  if (!cell_thread_reset(thread_count)) { halt_table_init_failure("thread"); }
  cell_snapshot_reset();
  if (!cell_fd_table_reset(open_file_count)) { halt_table_init_failure("open-file"); }
  cell_pipe_reset();
  cell_accounting_reset();
  cell_tty_reset();
  kprintf("[spore] process tables: domains=%u threads=%u mms=%u open-files=%u\n", (unsigned)domain_count,
          (unsigned)thread_count, (unsigned)mm_count, (unsigned)open_file_count);
}

bool cell_create_init(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp) {
  struct domain *domain = cell_alloc_domain();
  if (domain == NULL) { return false; }
  struct thread *thread = cell_alloc_thread(domain);
  if (thread == NULL) {
    domain->used = false;
    return false;
  }
  domain->parent_id = 0;
  domain->pgrp_id = domain->id;
  domain->session_id = domain->id;
  (void)cell_tty_set_foreground_pgrp(domain->pgrp_id);
  static const char *init_argv[] = {"/sbin/init"};
  cell_set_domain_identity(domain, "/sbin/init", init_argv, 1);
  struct process_mm *mm = cell_mm_from_owned(as, vmas);
  if (mm == NULL || !cell_domain_set_mm(domain, mm)) {
    if (mm != NULL) { cell_mm_release(mm); }
    thread->state = THREAD_UNUSED;
    domain->used = false;
    return false;
  }
  if (!vma_insert(cell_domain_vmas(domain), USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP,
                  VMM_USER_READ | VMM_USER_WRITE, 0, VMA_STACK)) {
    thread->state = THREAD_UNUSED;
    cell_mm_release(domain->mm);
    domain->mm = NULL;
    domain->used = false;
    return false;
  }
  if (!cell_init_stdio(domain)) {
    thread->state = THREAD_UNUSED;
    cell_mm_release(domain->mm);
    domain->mm = NULL;
    domain->used = false;
    return false;
  }
  thread->state = THREAD_RUNNABLE;
  aarch64_init_user_frame(&thread->tf, entry, sp);
  cell_set_current_thread(thread);
  kprintf("[spore] booting... domain %d / thread %d\n", domain->id, thread->tid);
  return true;
}
