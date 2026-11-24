#include "init/app.h"
#include "proc/snapshot.h"

#include "arch/aarch64/regs.h"
#include "arch/aarch64/smp.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/process.h"
#include "proc/thread.h"

struct snapshot {
  bool used;
  int id;
  struct user_address_space as;
  struct vma_list vmas;
};

static struct snapshot snapshots[MAX_SNAPSHOTS];
static int next_snapshot_id;

static struct snapshot *find_snapshot(int id) {
  for (size_t i = 0; i < MAX_SNAPSHOTS; ++i) {
    if (snapshots[i].used && snapshots[i].id == id) { return &snapshots[i]; }
  }
  return NULL;
}

static struct snapshot *alloc_snapshot(void) {
  for (size_t i = 0; i < MAX_SNAPSHOTS; ++i) {
    if (!snapshots[i].used) {
      kmemset(&snapshots[i], 0, sizeof(snapshots[i]));
      snapshots[i].used = true;
      snapshots[i].id = next_snapshot_id++;
      return &snapshots[i];
    }
  }
  return NULL;
}

void cell_snapshot_reset(void) {
  kmemset(snapshots, 0, sizeof(snapshots));
  next_snapshot_id = 0;
}

int snapshot_create_current(void) {
  if (app_mode) { return -1; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -12; }
  struct snapshot *snap = alloc_snapshot();
  if (snap == NULL) { return -12; }
  struct user_address_space *as = cell_domain_as(domain);
  struct vma_list *vmas = cell_domain_vmas(domain);
  if (as == NULL || vmas == NULL || !vmm_clone(&snap->as, as, VMM_SNAPSHOT)) {
    snap->used = false;
    return -12;
  }
  if (!vma_clone(&snap->vmas, vmas)) {
    vmm_destroy(&snap->as);
    snap->used = false;
    return -12;
  }
  return snap->id;
}

int snapshot_spawn(int snap_id, uint64_t entry, uint64_t arg, struct trap_frame *frame) {
  if (app_mode) { return -1; }
  struct domain *parent = cell_current_domain_internal();
  struct snapshot *snap = find_snapshot(snap_id);
  if (parent == NULL || snap == NULL) { return -12; }
  struct thread *child_thread = cell_alloc_process();
  if (child_thread == NULL) { return -12; }
  struct domain *child_domain = child_thread->domain;
  struct user_address_space child_as;
  struct vma_list child_vmas;
  if (!vmm_clone(&child_as, &snap->as, VMM_SNAPSHOT)) {
    cell_destroy_domain(child_domain);
    return -12;
  }
  if (!vma_clone(&child_vmas, &snap->vmas)) {
    vmm_destroy(&child_as);
    cell_destroy_domain(child_domain);
    return -12;
  }
  struct process_mm *child_mm = cell_mm_from_owned(&child_as, &child_vmas);
  if (child_mm == NULL || !cell_domain_set_mm(child_domain, child_mm)) {
    if (child_mm != NULL) { cell_mm_release(child_mm); }
    vmm_destroy(&child_as);
    vma_list_destroy(&child_vmas);
    cell_destroy_domain(child_domain);
    return -12;
  }
  cell_copy_domain_metadata(child_domain, parent);
  if (!cell_copy_fd_table(child_domain, parent)) { cell_destroy_domain(child_domain); return -12; }
  child_domain->parent_id = parent->id;
  child_thread->state = THREAD_RUNNABLE;
  cell_save_current(frame);
  child_thread->tf = *frame;
  child_thread->context = cell_current_thread_internal()->context;
  aarch64_set_snapshot_entry(&child_thread->tf, entry, arg, (uint64_t)child_domain->id);
  smp_notify();
  return child_domain->id;
}

int snapshot_reap(int pid, uint64_t status_addr, struct trap_frame *frame) {
  return cell_wait4_options(pid, status_addr, 0, 0, frame);
}
