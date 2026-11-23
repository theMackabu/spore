#pragma once

#include "proc/limits.h"
#include "proc/policy.h"
#include "proc/signal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct open_file;
struct process_mm;

constexpr int DOMAIN_GROUP_LIMIT = 32;

struct domain {
  int id;
  int parent_id;
  int pgrp_id;
  int session_id;
  uint16_t refcount;
  bool used;
  bool zombie;
  int exit_status;
  int term_signal;
  uint32_t uid;
  uint32_t euid;
  uint32_t gid;
  uint32_t egid;
  uint32_t saved_uid;
  uint32_t saved_gid;
  uint32_t groups[DOMAIN_GROUP_LIMIT];
  unsigned group_count;
  uint64_t start_ticks;
  uint64_t cpu_ticks;
  uint64_t alarm_deadline;
  uint64_t alarm_interval;
  uint64_t minor_faults;
  uint64_t major_faults;
  uint64_t unsupported_syscalls;
  uint64_t last_unsupported_syscall;
  uint64_t unsupported_ioctls;
  uint64_t last_unsupported_ioctl;
  struct process_mm *mm;
  struct open_file *fds[MAX_FDS];
  uint8_t fd_flags[MAX_FDS];
  char name[32];
  char exec_path[CELL_PATH_MAX];
  char argv0[64];
  char cmdline[160];
  char cwd[CELL_PATH_MAX];
  char fs_root[CELL_PATH_MAX];
  char chroot[CELL_PATH_MAX];
  struct capability_set caps;
  struct cpu_budget budget;
  struct signal_action signal_actions[65];
  uint64_t pending_signals;
};

bool cell_domain_reset(size_t capacity);
size_t cell_domain_capacity(void);
size_t cell_domain_index(const struct domain *domain);
uint32_t cell_domain_last_id(void);
void cell_copy_domain_metadata(struct domain *dst, const struct domain *src);
void cell_destroy_domain(struct domain *domain);
void cell_set_domain_identity(struct domain *domain, const char *path, const char *const argv[], uint64_t argc);
int cell_current_pid(void);
int cell_current_ppid(void);
const char *cell_current_name(void);
void cell_set_current_name(const char *name);
uint32_t cell_current_uid(void);
uint32_t cell_current_euid(void);
uint32_t cell_current_gid(void);
uint32_t cell_current_egid(void);
bool cell_in_group(uint32_t gid);
int cell_setres_ids(bool group, uint32_t real, uint32_t effective, uint32_t saved);
int cell_setuid_current(uint32_t uid);
int cell_setgid_current(uint32_t gid);
void cell_apply_exec_creds(uint32_t mode, uint32_t uid, uint32_t gid);
const char *cell_current_cwd(void);
bool cell_set_cwd(const char *path);
const char *cell_current_fs_root(void);
const char *cell_current_chroot(void);
bool cell_set_chroot(const char *path);
int cell_getpgid(int pid);
int cell_setpgid(int pid, int pgid);
int cell_getsid(int pid);
int cell_setsid_current(void);
bool cell_proc_exists(int pid);
int cell_proc_pid_at(size_t index);
uint32_t cell_proc_uid(int pid);
uint32_t cell_proc_gid(int pid);
void cell_note_unsupported_syscall(uint64_t nr);
void cell_note_unsupported_ioctl(uint64_t request);
struct domain *cell_current_domain_internal(void);
struct domain *cell_domain_slot(size_t index);
struct domain *cell_find_domain(int id);
struct domain *cell_alloc_domain(void);
