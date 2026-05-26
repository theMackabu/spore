#include "proc/domain.h"

#include "kstr.h"
#include "mem.h"
#include "proc/fd.h"
#include "proc/thread.h"
#include "proc/tty.h"

enum {
  EPERM = 1,
  ESRCH = 3,
  EINVAL = 22,
};

static struct domain domains[MAX_DOMAINS];
static int next_domain_id = 1;

void cell_domain_reset(void) {
  kmemset(domains, 0, sizeof(domains));
  next_domain_id = 1;
}

struct domain *cell_domain_slot(size_t index) {
  return index < MAX_DOMAINS ? &domains[index] : NULL;
}

size_t cell_domain_index(const struct domain *domain) {
  return domain == NULL ? MAX_DOMAINS : (size_t)(domain - domains);
}

uint32_t cell_domain_last_id(void) {
  return (uint32_t)(next_domain_id - 1);
}

struct domain *cell_find_domain(int id) {
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    if (domains[i].used && domains[i].id == id) { return &domains[i]; }
  }
  return NULL;
}

struct domain *cell_alloc_domain(void) {
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    if (!domains[i].used) {
      kmemset(&domains[i], 0, sizeof(domains[i]));
      domains[i].used = true;
      domains[i].refcount = 0;
      domains[i].id = next_domain_id++;
      domains[i].pgrp_id = domains[i].id;
      domains[i].session_id = domains[i].id;
      domains[i].uid = 0;
      domains[i].euid = 0;
      domains[i].gid = 0;
      domains[i].egid = 0;
      domains[i].start_ticks = cell_uptime_ticks();
      copy_cstr(domains[i].name, sizeof(domains[i].name), "?");
      copy_cstr(domains[i].exec_path, sizeof(domains[i].exec_path), "");
      copy_cstr(domains[i].argv0, sizeof(domains[i].argv0), "");
      copy_cstr(domains[i].cmdline, sizeof(domains[i].cmdline), "");
      domains[i].cwd[0] = '/';
      domains[i].cwd[1] = '\0';
      domains[i].fs_root[0] = '/';
      domains[i].fs_root[1] = '\0';
      domains[i].chroot[0] = '/';
      domains[i].chroot[1] = '\0';
      return &domains[i];
    }
  }
  return NULL;
}

void cell_copy_domain_metadata(struct domain *dst, const struct domain *src) {
  kmemcpy(dst->cwd, src->cwd, sizeof(dst->cwd));
  kmemcpy(dst->fs_root, src->fs_root, sizeof(dst->fs_root));
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
  dst->pgrp_id = src->pgrp_id;
  dst->session_id = src->session_id;
  kmemcpy(dst->signal_actions, src->signal_actions, sizeof(dst->signal_actions));
  dst->start_ticks = cell_uptime_ticks();
  dst->cpu_ticks = 0;
}

void cell_destroy_domain(struct domain *domain) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain) {
      thread->state = THREAD_UNUSED;
      thread->domain = NULL;
    }
  }
  cell_close_all_fds(domain);
  vmm_destroy(&domain->as);
  vma_list_destroy(&domain->vmas);
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

struct user_address_space *cell_current_as(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? NULL : &domain->as;
}

int cell_current_pid(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? 0 : domain->id;
}

int cell_current_tid(void) {
  struct thread *thread = cell_current_thread_internal();
  return thread == NULL ? 0 : thread->tid;
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
  (void)cell_tty_set_foreground_pgrp(domain->pgrp_id);
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

int cell_setuid_current(uint32_t uid) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -1; }
  if (domain->euid != 0 && uid != domain->uid) { return -1; }
  domain->uid = uid;
  domain->euid = uid;
  return 0;
}

int cell_setgid_current(uint32_t gid) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -1; }
  if (domain->euid != 0 && gid != domain->gid) { return -1; }
  domain->gid = gid;
  domain->egid = gid;
  return 0;
}

void cell_apply_exec_creds(uint32_t mode, uint32_t uid, uint32_t gid) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return; }
  if ((mode & 04000u) != 0) { domain->euid = uid; }
  if ((mode & 02000u) != 0) { domain->egid = gid; }
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

const char *cell_current_fs_root(void) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL ? "/" : domain->fs_root;
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
