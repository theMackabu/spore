#pragma once

#include "arch/aarch64/regs.h"
#include "elf/loader.h"
#include "mm/vmm.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    MAX_CELLS = 16,
    MAX_SNAPSHOTS = 8,
    CELL_SWITCHED = -0x40000000,
};

enum cell_state {
    CELL_UNUSED,
    CELL_RUNNABLE,
    CELL_BLOCKED,
    CELL_ZOMBIE,
};

struct cell {
    int pid;
    int parent_pid;
    enum cell_state state;
    struct user_address_space as;
    struct trap_frame tf;
    uint64_t tpidr_el0;
    int exit_status;
    int wait_target;
};

struct snapshot {
    bool used;
    int id;
    struct user_address_space as;
};

void cell_system_init(uint64_t hhdm_offset);
bool cell_create_init(struct user_address_space *as, uint64_t entry, uint64_t sp);
struct user_address_space *cell_current_as(void);
int cell_current_pid(void);
int cell_current_ppid(void);
void cell_save_current(const struct trap_frame *frame);
void cell_restore_current(struct trap_frame *frame);
void cell_schedule(struct trap_frame *frame);
void cell_exit_current(int status, struct trap_frame *frame);
int cell_fork_current(struct trap_frame *frame);
int cell_wait4(int pid, uint64_t status_addr, struct trap_frame *frame);
int cell_kill(int pid, int signal);
bool cell_handle_cow_fault(uint64_t far);
int snapshot_create_current(void);
int snapshot_spawn(int snap_id, uint64_t entry, uint64_t arg);
int snapshot_reap(int pid, uint64_t status_addr, struct trap_frame *frame);
