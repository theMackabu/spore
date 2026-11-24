#include "proc/domain.h"
#include "proc/accounting.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/fd.h"
#include "proc/limits.h"
#include "proc/memory.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "tty/tty.h"

enum {
  EPERM = 1,
  ESRCH = 3,
  EINVAL = 22,
};

static struct domain *domains;
static size_t domain_capacity;
static size_t domain_slot_limit;
static int next_domain_id = 1;

bool cell_domain_reset(size_t capacity) {
  if (capacity == 0 || capacity > MAX_DOMAINS) { return false; }
  if (domains == NULL || domain_capacity != capacity) {
    domains = pmm_alloc_table(capacity, sizeof(*domains));
    if (domains == NULL) { return false; }
    domain_capacity = capacity;
  } else {
    kmemset(domains, 0, domain_capacity * sizeof(*domains));
  }
  domain_slot_limit = 0;
  next_domain_id = 1;
  return true;
}

size_t cell_domain_slot_limit(void) {
  return domain_slot_limit;
}

struct domain *cell_domain_slot(size_t index) {
  return index < domain_capacity ? &domains[index] : NULL;
}

uint32_t cell_domain_last_id(void) {
  return (uint32_t)(next_domain_id - 1);
}

struct domain *cell_find_domain(int id) {
  for (size_t i = 0; i < domain_slot_limit; ++i) {
    if (domains[i].used && domains[i].id == id) { return &domains[i]; }
  }
  return NULL;
}

struct domain *cell_alloc_domain(void) {
  for (size_t i = 0; i < domain_capacity; ++i) {
    if (!domains[i].used) {
      kmemset(&domains[i], 0, sizeof(domains[i]));
      if (i == domain_slot_limit) { ++domain_slot_limit; }
      domains[i].used = true;
      domains[i].id = next_domain_id++;
      domains[i].pgrp_id = domains[i].id;
      domains[i].session_id = domains[i].id;
      domains[i].start_ticks = cell_uptime_ticks();
      copy_cstr(domains[i].name, sizeof(domains[i].name), "?");
      domains[i].cwd[0] = '/';
      domains[i].chroot[0] = '/';
      return &domains[i];
    }
  }
  return NULL;
}

void cell_copy_domain_metadata(struct domain *dst, const struct domain *src) {
  kmemcpy(dst->cwd, src->cwd, sizeof(dst->cwd));
  kmemcpy(dst->chroot, src->chroot, sizeof(dst->chroot));
  kmemcpy(dst->name, src->name, sizeof(dst->name));
  kmemcpy(dst->exec_path, src->exec_path, sizeof(dst->exec_path));
  kmemcpy(dst->argv0, src->argv0, sizeof(dst->argv0));
  kmemcpy(dst->cmdline, src->cmdline, sizeof(dst->cmdline));
  dst->caps = src->caps;
  dst->budget = src->budget;
  dst->uid = src->uid;
  dst->euid = src->euid;
  dst->gid = src->gid;
  dst->egid = src->egid;
  dst->saved_uid = src->saved_uid;
  dst->saved_gid = src->saved_gid;
  dst->group_count = src->group_count;
  kmemcpy(dst->groups, src->groups, src->group_count * sizeof(src->groups[0]));
  dst->pgrp_id = src->pgrp_id;
  dst->session_id = src->session_id;
  kmemcpy(dst->signal_actions, src->signal_actions, sizeof(dst->signal_actions));
  dst->start_ticks = cell_uptime_ticks();
  dst->cpu_ticks = 0;
}

void cell_destroy_domain(struct domain *domain) {
  int reparent_to = (domain != NULL && domain->id != 1 && cell_find_domain(1) != NULL) ? 1 : 0;
  if (domain != NULL) {
    for (size_t i = 0, limit = cell_domain_slot_limit(); i < limit; ++i) {
      struct domain *child = cell_domain_slot(i);
      if (child == NULL || !child->used || child->parent_id != domain->id) { continue; }
      child->parent_id = reparent_to;
      if (child->zombie && reparent_to != 0) { cell_wake_parent_of(child); }
    }
  }
  for (size_t i = 0, limit = cell_thread_slot_limit(); i < limit; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain) {
      cell_clear_wait(thread);
      thread->state = THREAD_UNUSED;
      thread->running_cpu = -1;
      thread->domain = NULL;
    }
  }
  cell_close_all_fds(domain);
  cell_mm_release(domain->mm);
  domain->mm = NULL;
  domain->used = false;
  domain->zombie = false;
  domain->refcount = 0;
}

void cell_set_domain_identity(struct domain *domain, const char *path, const char *const argv[], uint64_t argc) {
  if (domain == NULL) { return; }
  const char *argv0 = argc > 0 && argv != NULL && argv[0] != NULL && argv[0][0] != '\0' ? argv[0] : path;
  copy_cstr(domain->exec_path, sizeof(domain->exec_path), path == NULL ? "" : path);
  copy_cstr(domain->argv0, sizeof(domain->argv0), argv0 == NULL ? "" : argv0);
  copy_cstr(domain->name, sizeof(domain->name), base_name(argv0 == NULL || argv0[0] == '\0' ? path : argv0));
  domain->cmdline[0] = '\0';
  size_t len = 0;
  for (uint64_t i = 0; i < argc && argv != NULL && argv[i] != NULL; ++i) {
    if (i != 0 && len + 1 < sizeof(domain->cmdline)) { domain->cmdline[len++] = ' '; }
    const char *arg = argv[i];
    while (*arg != '\0' && len + 1 < sizeof(domain->cmdline)) {
      domain->cmdline[len++] = *arg++;
    }
  }
  if (len == 0) {
    copy_cstr(domain->cmdline, sizeof(domain->cmdline), domain->argv0);
  } else {
    domain->cmdline[len] = '\0';
  }
}

int cell_current_pid(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? 0 : domain->id;
}

int cell_current_ppid(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? 0 : domain->parent_id;
}

const char *cell_current_name(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? "-" : domain->name;
}

void cell_set_current_name(const char *name) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return; }
  copy_cstr(domain->name, sizeof(domain->name), name);
}

int cell_getpgid(int pid) {
  struct domain *domain = pid == 0 ? cell_current_domain_internal() : cell_find_domain(pid);
  return domain == NULL ? -ESRCH : domain->pgrp_id;
}

int cell_setpgid(int pid, int pgid) {
  struct domain *current = cell_current_domain_internal();
  if (current == NULL) { return -ESRCH; }
  struct domain *target = pid == 0 ? current : cell_find_domain(pid);
  if (target == NULL) { return -ESRCH; }
  if (target != current && target->parent_id != current->id) { return -EPERM; }
  if (pgid == 0) { pgid = target->id; }
  if (pgid < 0) { return -EINVAL; }
  target->pgrp_id = pgid;
  return 0;
}

int cell_getsid(int pid) {
  struct domain *domain = pid == 0 ? cell_current_domain_internal() : cell_find_domain(pid);
  return domain == NULL ? -ESRCH : domain->session_id;
}

int cell_setsid_current(void) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -ESRCH; }
  if (domain->pgrp_id == domain->id) { return -EPERM; }
  domain->session_id = domain->id;
  domain->pgrp_id = domain->id;
  return domain->session_id;
}

uint32_t cell_current_uid(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? 0 : domain->uid;
}

uint32_t cell_current_euid(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? 0 : domain->euid;
}

uint32_t cell_current_gid(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? 0 : domain->gid;
}

uint32_t cell_current_egid(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? 0 : domain->egid;
}

bool cell_in_group(uint32_t gid) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return false; }
  if (gid == domain->egid) { return true; }
  for (unsigned i = 0; i < domain->group_count; ++i) {
    if (domain->groups[i] == gid) { return true; }
  }
  return false;
}

static bool held_id(uint32_t id, uint32_t real, uint32_t effective, uint32_t saved) {
  return id == UINT32_MAX || id == real || id == effective || id == saved;
}

int cell_setres_ids(bool group, uint32_t real, uint32_t effective, uint32_t saved) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -ESRCH; }
  uint32_t *real_id = group ? &domain->gid : &domain->uid;
  uint32_t *effective_id = group ? &domain->egid : &domain->euid;
  uint32_t *saved_id = group ? &domain->saved_gid : &domain->saved_uid;
  if (domain->euid != 0 && (!held_id(real, *real_id, *effective_id, *saved_id) || !held_id(effective, *real_id, *effective_id, *saved_id) ||
                        !held_id(saved, *real_id, *effective_id, *saved_id))) { return -EPERM; }
  if (real != UINT32_MAX) { *real_id = real; }
  if (effective != UINT32_MAX) { *effective_id = effective; }
  if (saved != UINT32_MAX) { *saved_id = saved; }
  return 0;
}

int cell_setuid_current(uint32_t uid) {
  if (uid == UINT32_MAX) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -ESRCH; }
  if (domain->euid != 0 && uid != domain->uid && uid != domain->saved_uid) { return -EPERM; }
  if (domain->euid == 0) { return cell_setres_ids(false, uid, uid, uid); }
  return cell_setres_ids(false, UINT32_MAX, uid, UINT32_MAX);
}

int cell_setgid_current(uint32_t gid) {
  if (gid == UINT32_MAX) { return -EINVAL; }
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -ESRCH; }
  if (domain->euid != 0 && gid != domain->gid && gid != domain->saved_gid) { return -EPERM; }
  if (domain->euid == 0) { return cell_setres_ids(true, gid, gid, gid); }
  return cell_setres_ids(true, UINT32_MAX, gid, UINT32_MAX);
}

void cell_apply_exec_creds(uint32_t mode, uint32_t uid, uint32_t gid) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return; }
  if ((mode & 04000u) != 0) { domain->euid = uid; }
  if ((mode & 02000u) != 0) { domain->egid = gid; }
  domain->saved_uid = domain->euid;
  domain->saved_gid = domain->egid;
}

const char *cell_current_cwd(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? "/" : domain->cwd;
}

bool cell_set_cwd(const char *path) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || path == NULL || path[0] != '/') { return false; }
  size_t len = kstrlen(path);
  if (len >= sizeof(domain->cwd)) { return false; }
  kmemcpy(domain->cwd, path, len + 1);
  return true;
}

const char *cell_current_chroot(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? "/" : domain->chroot;
}

bool cell_set_chroot(const char *path) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || path == NULL || path[0] != '/') { return false; }
  size_t len = kstrlen(path);
  if (len >= sizeof(domain->chroot)) { return false; }
  kmemcpy(domain->chroot, path, len + 1);
  return true;
}

bool cell_proc_exists(int pid) {
  return cell_find_domain(pid) != NULL;
}

uint32_t cell_proc_uid(int pid) {
  struct domain *domain = cell_find_domain(pid);
  return domain == NULL ? 0 : domain->uid;
}

uint32_t cell_proc_gid(int pid) {
  struct domain *domain = cell_find_domain(pid);
  return domain == NULL ? 0 : domain->gid;
}

void cell_note_unsupported_syscall(uint64_t nr) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return; }
  ++domain->unsupported_syscalls;
  domain->last_unsupported_syscall = nr;
}

void cell_note_unsupported_ioctl(uint64_t request) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return; }
  ++domain->unsupported_ioctls;
  domain->last_unsupported_ioctl = request;
}
