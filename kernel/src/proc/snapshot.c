#include "proc/snapshot.h"

#include "mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/thread.h"

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
  struct domain *domain = cell_current_domain_internal();
  struct snapshot *snap = alloc_snapshot();
  if (domain == NULL || snap == NULL) { return -12; }
  if (!vmm_clone_cow(&snap->as, &domain->as, 0)) {
    snap->used = false;
    return -12;
  }
  if (!vma_clone(&snap->vmas, &domain->vmas)) {
    vmm_destroy(&snap->as);
    snap->used = false;
    return -12;
  }
  return snap->id;
}

int snapshot_spawn(int snap_id, uint64_t entry, uint64_t arg, struct trap_frame *frame) {
  struct domain *parent = cell_current_domain_internal();
  struct snapshot *snap = find_snapshot(snap_id);
  struct domain *child_domain = cell_alloc_domain();
  if (parent == NULL || snap == NULL || child_domain == NULL) { return -12; }
  struct thread *child_thread = cell_alloc_thread(child_domain);
  if (child_thread == NULL) {
    child_domain->used = false;
    return -12;
  }
  if (!vmm_clone_cow(&child_domain->as, &snap->as, 0)) {
    child_thread->state = THREAD_UNUSED;
    child_domain->used = false;
    return -12;
  }
  if (!vma_clone(&child_domain->vmas, &snap->vmas)) {
    vmm_destroy(&child_domain->as);
    child_thread->state = THREAD_UNUSED;
    child_domain->used = false;
    return -12;
  }
  cell_copy_domain_metadata(child_domain, parent);
  cell_copy_fd_table(child_domain, parent);
  child_domain->parent_id = parent->id;
  child_thread->state = THREAD_RUNNABLE;
  cell_save_current(frame);
  child_thread->tf = *frame;
  child_thread->fp = cell_current_thread_internal()->fp;
  child_thread->tf.elr_el1 = entry;
  child_thread->tf.x[0] = arg;
  child_thread->tf.x[1] = (uint64_t)child_domain->id;
  child_thread->tf.spsr_el1 &= ~(1ull << 7);
  child_thread->tpidr_el0 = cell_current_thread_internal()->tpidr_el0;
  return child_domain->id;
}

int snapshot_reap(int pid, uint64_t status_addr, struct trap_frame *frame) {
  return cell_wait4(pid, status_addr, frame);
}
