#pragma once

#include <stdbool.h>
#include <stdint.h>

constexpr uint64_t CLONE_PARENT = 0x00008000;

struct domain;
struct trap_frame;
struct user_address_space;
struct vma_list;

void cell_reparent_children(struct domain *parent, int exiting_tid, int survivor_tid);
void cell_reap_namespace_orphan(struct domain *child);
void cell_wake_parent_of(struct domain *child);
void cell_exit_thread_current(int status, struct trap_frame *frame);
void cell_exit_group_current(int status, struct trap_frame *frame);
int cell_fork_current(struct trap_frame *frame, uint64_t flags, uint64_t newsp, uint64_t parent_tid,
                      uint64_t child_tid);
int cell_vfork_current(struct trap_frame *frame, uint64_t newsp, uint64_t flags, uint64_t parent_tid, uint64_t tls,
                       uint64_t child_tid);
int cell_clone_thread_current(struct trap_frame *frame, uint64_t flags, uint64_t newsp, uint64_t parent_tid,
                              uint64_t tls, uint64_t child_tid);
int cell_set_tid_address_current(uint64_t clear_child_tid);
int cell_wait4_options(int pid, uint64_t status_addr, int options, uint64_t usage_addr, struct trap_frame *frame);
bool cell_exec_replace(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp,
                       struct trap_frame *frame, const char *path, const char *const argv[], uint64_t argc);
