#pragma once

#include <stdint.h>
#include <stdbool.h>

struct trap_frame;

int64_t sys_set_ids(bool group, bool single, uint32_t real, uint32_t effective, uint32_t saved);
int64_t sys_getres_ids(bool group, uint64_t real, uint64_t effective, uint64_t saved);
int64_t sys_getgroups(int count, uint64_t list);
int64_t sys_capabilities(bool set, uint64_t header, uint64_t data);
int64_t sys_setgroups(uint64_t count, uint64_t list);
int64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_limit, uint64_t old_limit);
int64_t sys_execve(struct trap_frame *frame, uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr);
int64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);
int64_t sys_sched_getparam(int tid, uint64_t param);
int64_t sys_sched_getscheduler(int tid);
int64_t sys_getrusage(int who, uint64_t usage_addr);
int64_t sys_spore_procinfo(uint64_t addr, uint64_t bytes);
int64_t sys_clone(struct trap_frame *frame, uint64_t flags, uint64_t newsp, uint64_t parent_tid, uint64_t tls,
                  uint64_t child_tid);
int64_t sys_futex(struct trap_frame *frame, uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout, uint64_t uaddr2, uint32_t bitset);
int64_t sys_sched_getaffinity(uint64_t mask, uint64_t len);
int cell_rt_sigaction(int signal, uint64_t act_addr, uint64_t old_addr, uint64_t sigset_size);
int64_t sys_sigaltstack(struct trap_frame *frame, uint64_t new_addr, uint64_t old_addr);
int64_t sys_rt_sigprocmask(struct trap_frame *frame, uint64_t how, uint64_t set, uint64_t oldset, uint64_t sigsetsize);
int64_t sys_getcpu(uint64_t cpu_addr, uint64_t node_addr);
