#include "cell.h"

#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"

static struct cell cells[MAX_CELLS];
static struct snapshot snapshots[MAX_SNAPSHOTS];
static struct cell *current_cell;
static uint64_t kernel_hhdm;
static int next_pid = 1;
static int next_snapshot_id;

static void poweroff(void) {
    __asm__ volatile(
        "mov x0, #0x0008\n"
        "movk x0, #0x8400, lsl #16\n"
        "hvc #0\n"
        :
        :
        : "x0", "memory");
}

static struct cell *find_cell(int pid) {
    for (size_t i = 0; i < MAX_CELLS; ++i) {
        if (cells[i].state != CELL_UNUSED && cells[i].pid == pid) {
            return &cells[i];
        }
    }
    return NULL;
}

static struct cell *alloc_cell(void) {
    for (size_t i = 0; i < MAX_CELLS; ++i) {
        if (cells[i].state == CELL_UNUSED) {
            kmemset(&cells[i], 0, sizeof(cells[i]));
            cells[i].pid = next_pid++;
            cells[i].wait_target = -1;
            return &cells[i];
        }
    }
    return NULL;
}

static struct snapshot *find_snapshot(int id) {
    for (size_t i = 0; i < MAX_SNAPSHOTS; ++i) {
        if (snapshots[i].used && snapshots[i].id == id) {
            return &snapshots[i];
        }
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

static void wake_parent_of(const struct cell *child) {
    struct cell *parent = find_cell(child->parent_pid);
    if (parent != NULL && parent->state == CELL_BLOCKED &&
        (parent->wait_target < 0 || parent->wait_target == child->pid)) {
        int status = child->exit_status << 8;
        uint64_t status_addr = parent->tf.x[1];
        if (status_addr != 0) {
            (void)vmm_copy_to_user(&parent->as, status_addr, &status, sizeof(status));
        }
        parent->tf.x[0] = (uint64_t)child->pid;
        vmm_destroy((struct user_address_space *)&child->as);
        ((struct cell *)child)->state = CELL_UNUSED;
        parent->state = CELL_RUNNABLE;
        parent->wait_target = -1;
    }
}

void cell_system_init(uint64_t hhdm_offset) {
    kernel_hhdm = hhdm_offset;
    kmemset(cells, 0, sizeof(cells));
    kmemset(snapshots, 0, sizeof(snapshots));
    current_cell = NULL;
    next_pid = 1;
    next_snapshot_id = 0;
    // v1 is cooperative and UP: cell table state has no locks. v2 preemption/SMP
    // must add synchronization here.
}

bool cell_create_init(struct user_address_space *as, uint64_t entry, uint64_t sp) {
    struct cell *cell = alloc_cell();
    if (cell == NULL) {
        return false;
    }
    cell->parent_pid = 0;
    cell->state = CELL_RUNNABLE;
    cell->as = *as;
    cell->as.asid = 0;
    cell->tf.elr_el1 = entry;
    cell->tf.sp_el0 = sp;
    cell->tf.spsr_el1 = 0x3c0;
    current_cell = cell;
    return true;
}

struct user_address_space *cell_current_as(void) {
    return current_cell == NULL ? NULL : &current_cell->as;
}

int cell_current_pid(void) {
    return current_cell == NULL ? 0 : current_cell->pid;
}

int cell_current_ppid(void) {
    return current_cell == NULL ? 0 : current_cell->parent_pid;
}

void cell_save_current(const struct trap_frame *frame) {
    if (current_cell == NULL) {
        return;
    }
    current_cell->tf = *frame;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(current_cell->tpidr_el0));
}

void cell_restore_current(struct trap_frame *frame) {
    if (current_cell == NULL) {
        return;
    }
    vmm_install_user(&current_cell->as);
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(current_cell->tpidr_el0));
    *frame = current_cell->tf;
}

void cell_schedule(struct trap_frame *frame) {
    cell_save_current(frame);
    if (current_cell != NULL && current_cell->state == CELL_RUNNABLE) {
        size_t start = (size_t)(current_cell - cells + 1);
        for (size_t n = 0; n < MAX_CELLS; ++n) {
            struct cell *candidate = &cells[(start + n) % MAX_CELLS];
            if (candidate->state == CELL_RUNNABLE) {
                current_cell = candidate;
                cell_restore_current(frame);
                return;
            }
        }
    }
    for (size_t i = 0; i < MAX_CELLS; ++i) {
        if (cells[i].state == CELL_RUNNABLE) {
            current_cell = &cells[i];
            cell_restore_current(frame);
            return;
        }
    }
    kprintf("[kernel] no runnable cells\n");
    poweroff();
    for (;;) {
        __asm__ volatile("wfe");
    }
}

void cell_exit_current(int status, struct trap_frame *frame) {
    if (current_cell == NULL) {
        return;
    }
    current_cell->exit_status = status;
    current_cell->state = CELL_ZOMBIE;
    wake_parent_of(current_cell);
    cell_schedule(frame);
}

int cell_fork_current(struct trap_frame *frame) {
    struct cell *child = alloc_cell();
    if (child == NULL) {
        return -12;
    }
    cell_save_current(frame);
    if (!vmm_clone_cow(&child->as, &current_cell->as, 0)) {
        child->state = CELL_UNUSED;
        return -12;
    }
    child->parent_pid = current_cell->pid;
    child->state = CELL_RUNNABLE;
    child->tf = current_cell->tf;
    child->tf.x[0] = 0;
    child->tpidr_el0 = current_cell->tpidr_el0;
    current_cell->tf.x[0] = (uint64_t)child->pid;
    return child->pid;
}

static struct cell *find_waitable_child(int parent_pid, int pid) {
    for (size_t i = 0; i < MAX_CELLS; ++i) {
        if (cells[i].state == CELL_ZOMBIE && cells[i].parent_pid == parent_pid &&
            (pid <= 0 || cells[i].pid == pid)) {
            return &cells[i];
        }
    }
    return NULL;
}

static bool has_child(int parent_pid, int pid) {
    for (size_t i = 0; i < MAX_CELLS; ++i) {
        if (cells[i].state != CELL_UNUSED && cells[i].parent_pid == parent_pid &&
            (pid <= 0 || cells[i].pid == pid)) {
            return true;
        }
    }
    return false;
}

int cell_wait4(int pid, uint64_t status_addr, struct trap_frame *frame) {
    struct cell *child = find_waitable_child(current_cell->pid, pid);
    if (child == NULL) {
        if (!has_child(current_cell->pid, pid)) {
            return -10;
        }
        cell_save_current(frame);
        current_cell->state = CELL_BLOCKED;
        current_cell->wait_target = pid;
        cell_schedule(frame);
        return CELL_SWITCHED;
    }

    int status = child->exit_status << 8;
    if (status_addr != 0 && !vmm_copy_to_user(&current_cell->as, status_addr, &status, sizeof(status))) {
        return -14;
    }
    int child_pid = child->pid;
    vmm_destroy(&child->as);
    child->state = CELL_UNUSED;
    current_cell->wait_target = -1;
    return child_pid;
}

int cell_kill(int pid, int signal) {
    (void)signal;
    struct cell *cell = find_cell(pid);
    if (cell == NULL || cell->state == CELL_UNUSED || cell->state == CELL_ZOMBIE) {
        return -3;
    }
    cell->exit_status = 128 + signal;
    cell->state = CELL_ZOMBIE;
    wake_parent_of(cell);
    return 0;
}

bool cell_handle_cow_fault(uint64_t far) {
    return current_cell != NULL && vmm_handle_cow_fault(&current_cell->as, far);
}

int snapshot_create_current(void) {
    struct snapshot *snap = alloc_snapshot();
    if (snap == NULL) {
        return -12;
    }
    if (!vmm_clone_cow(&snap->as, &current_cell->as, 0)) {
        snap->used = false;
        return -12;
    }
    return snap->id;
}

int snapshot_spawn(int snap_id, uint64_t entry, uint64_t arg) {
    struct snapshot *snap = find_snapshot(snap_id);
    struct cell *child = alloc_cell();
    if (snap == NULL || child == NULL) {
        return -12;
    }
    if (!vmm_clone_cow(&child->as, &snap->as, 0)) {
        child->state = CELL_UNUSED;
        return -12;
    }
    child->parent_pid = current_cell->pid;
    child->state = CELL_RUNNABLE;
    child->tf = current_cell->tf;
    child->tf.elr_el1 = entry;
    child->tf.x[0] = arg;
    child->tf.x[1] = (uint64_t)child->pid;
    child->tpidr_el0 = current_cell->tpidr_el0;
    return child->pid;
}

int snapshot_reap(int pid, uint64_t status_addr, struct trap_frame *frame) {
    return cell_wait4(pid, status_addr, frame);
}
