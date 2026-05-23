#include "cell.h"

#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "pl011.h"

static struct domain domains[MAX_DOMAINS];
static struct thread threads[MAX_THREADS];
static struct snapshot snapshots[MAX_SNAPSHOTS];
static struct open_file open_files[MAX_OPEN_FILES];
static struct thread *current_thread;
static int next_domain_id = 1;
static int next_thread_id = 1;
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

static struct domain *current_domain(void) {
    return current_thread == NULL ? NULL : current_thread->domain;
}

static struct domain *find_domain(int id) {
    for (size_t i = 0; i < MAX_DOMAINS; ++i) {
        if (domains[i].used && domains[i].id == id) {
            return &domains[i];
        }
    }
    return NULL;
}

static struct domain *alloc_domain(void) {
    for (size_t i = 0; i < MAX_DOMAINS; ++i) {
        if (!domains[i].used) {
            kmemset(&domains[i], 0, sizeof(domains[i]));
            domains[i].used = true;
            domains[i].refcount = 1;
            domains[i].id = next_domain_id++;
            domains[i].cwd[0] = '/';
            domains[i].cwd[1] = '\0';
            domains[i].fs_root[0] = '/';
            domains[i].fs_root[1] = '\0';
            return &domains[i];
        }
    }
    return NULL;
}

static struct thread *alloc_thread(struct domain *domain) {
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (threads[i].state == THREAD_UNUSED) {
            kmemset(&threads[i], 0, sizeof(threads[i]));
            threads[i].tid = next_thread_id++;
            threads[i].domain = domain;
            threads[i].wait_target = -1;
            return &threads[i];
        }
    }
    return NULL;
}

static struct open_file *alloc_open_file(void) {
    for (size_t i = 0; i < MAX_OPEN_FILES; ++i) {
        if (!open_files[i].used) {
            kmemset(&open_files[i], 0, sizeof(open_files[i]));
            open_files[i].used = true;
            open_files[i].refcount = 1;
            return &open_files[i];
        }
    }
    return NULL;
}

static void retain_open_file(struct open_file *file) {
    if (file != NULL) {
        ++file->refcount;
    }
}

static void release_open_file(struct open_file *file) {
    if (file == NULL || file->refcount == 0) {
        return;
    }
    --file->refcount;
    if (file->refcount == 0) {
        file->used = false;
    }
}

static void close_all_fds(struct domain *domain) {
    for (size_t i = 0; i < MAX_FDS; ++i) {
        release_open_file(domain->fds[i]);
        domain->fds[i] = NULL;
    }
}

static bool init_stdio(struct domain *domain) {
    struct open_file *in = alloc_open_file();
    struct open_file *out = alloc_open_file();
    struct open_file *err = alloc_open_file();
    if (in == NULL || out == NULL || err == NULL) {
        release_open_file(in);
        release_open_file(out);
        release_open_file(err);
        return false;
    }
    in->type = OPEN_STDIN;
    out->type = OPEN_STDOUT;
    err->type = OPEN_STDOUT;
    domain->fds[0] = in;
    domain->fds[1] = out;
    domain->fds[2] = err;
    return true;
}

static void copy_fd_table(struct domain *dst, const struct domain *src) {
    for (size_t i = 0; i < MAX_FDS; ++i) {
        dst->fds[i] = src->fds[i];
        retain_open_file(dst->fds[i]);
    }
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

static struct thread *thread_for_domain(struct domain *domain) {
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (threads[i].state != THREAD_UNUSED && threads[i].domain == domain) {
            return &threads[i];
        }
    }
    return NULL;
}

static void destroy_domain(struct domain *domain) {
    close_all_fds(domain);
    vmm_destroy(&domain->as);
    domain->used = false;
    domain->zombie = false;
}

static void wake_parent_of(struct domain *child) {
    struct domain *parent_domain = find_domain(child->parent_id);
    struct thread *parent = parent_domain == NULL ? NULL : thread_for_domain(parent_domain);
    if (parent != NULL && parent->state == THREAD_BLOCKED &&
        (parent->wait_target < 0 || parent->wait_target == child->id)) {
        int status = child->exit_status << 8;
        uint64_t status_addr = parent->tf.x[1];
        if (status_addr != 0) {
            (void)vmm_copy_to_user(&parent_domain->as, status_addr, &status, sizeof(status));
        }
        parent->tf.x[0] = (uint64_t)child->id;
        struct thread *child_thread = thread_for_domain(child);
        if (child_thread != NULL) {
            child_thread->state = THREAD_UNUSED;
        }
        destroy_domain(child);
        parent->state = THREAD_RUNNABLE;
        parent->wait_target = -1;
    }
}

void cell_system_init(uint64_t hhdm_offset) {
    (void)hhdm_offset;
    kmemset(domains, 0, sizeof(domains));
    kmemset(threads, 0, sizeof(threads));
    kmemset(snapshots, 0, sizeof(snapshots));
    kmemset(open_files, 0, sizeof(open_files));
    current_thread = NULL;
    next_domain_id = 1;
    next_thread_id = 1;
    next_snapshot_id = 0;
    // v2 Phase A object model: domains own isolation/policy state, threads own
    // EL0 execution state. The kernel remains run-to-completion on one core, so
    // these tables intentionally have no locks until a later SMP/preemptive goal.
}

bool cell_create_init(struct user_address_space *as, uint64_t entry, uint64_t sp) {
    struct domain *domain = alloc_domain();
    if (domain == NULL) {
        return false;
    }
    struct thread *thread = alloc_thread(domain);
    if (thread == NULL) {
        domain->used = false;
        return false;
    }
    domain->parent_id = 0;
    domain->as = *as;
    domain->as.asid = 0;
    vma_list_init(&domain->vmas);
    if (!init_stdio(domain)) {
        thread->state = THREAD_UNUSED;
        domain->used = false;
        return false;
    }
    thread->state = THREAD_RUNNABLE;
    thread->tf.elr_el1 = entry;
    thread->tf.sp_el0 = sp;
    thread->tf.spsr_el1 = 0x3c0;
    current_thread = thread;
    kprintf("[spore] booting... domain %d / thread %d\n", domain->id, thread->tid);
    return true;
}

struct user_address_space *cell_current_as(void) {
    struct domain *domain = current_domain();
    return domain == NULL ? NULL : &domain->as;
}

int cell_current_pid(void) {
    struct domain *domain = current_domain();
    return domain == NULL ? 0 : domain->id;
}

int cell_current_tid(void) {
    return current_thread == NULL ? 0 : current_thread->tid;
}

int cell_current_ppid(void) {
    struct domain *domain = current_domain();
    return domain == NULL ? 0 : domain->parent_id;
}

void cell_save_current(const struct trap_frame *frame) {
    if (current_thread == NULL) {
        return;
    }
    current_thread->tf = *frame;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(current_thread->tpidr_el0));
}

static void restore_thread(struct thread *thread, struct trap_frame *frame, struct domain *old_domain) {
    if (old_domain != thread->domain) {
        vmm_install_user(&thread->domain->as);
    }
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(thread->tpidr_el0));
    *frame = thread->tf;
}

void cell_restore_current(struct trap_frame *frame) {
    if (current_thread == NULL) {
        return;
    }
    vmm_install_user(&current_thread->domain->as);
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(current_thread->tpidr_el0));
    *frame = current_thread->tf;
}

void cell_schedule(struct trap_frame *frame) {
    struct domain *old_domain = current_domain();
    cell_save_current(frame);
    if (current_thread != NULL && current_thread->state == THREAD_RUNNABLE) {
        size_t start = (size_t)(current_thread - threads + 1);
        for (size_t n = 0; n < MAX_THREADS; ++n) {
            struct thread *candidate = &threads[(start + n) % MAX_THREADS];
            if (candidate->state == THREAD_RUNNABLE) {
                current_thread = candidate;
                restore_thread(candidate, frame, old_domain);
                return;
            }
        }
    }
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (threads[i].state == THREAD_RUNNABLE) {
            current_thread = &threads[i];
            restore_thread(current_thread, frame, old_domain);
            return;
        }
    }
    kprintf("[kernel] no runnable threads\n");
    poweroff();
    for (;;) {
        __asm__ volatile("wfe");
    }
}

void cell_exit_current(int status, struct trap_frame *frame) {
    if (current_thread == NULL) {
        return;
    }
    struct domain *domain = current_thread->domain;
    domain->exit_status = status;
    domain->zombie = true;
    current_thread->state = THREAD_ZOMBIE;
    wake_parent_of(domain);
    cell_schedule(frame);
}

int cell_fork_current(struct trap_frame *frame) {
    struct domain *parent = current_domain();
    struct domain *child_domain = alloc_domain();
    if (parent == NULL || child_domain == NULL) {
        return -12;
    }
    struct thread *child_thread = alloc_thread(child_domain);
    if (child_thread == NULL) {
        child_domain->used = false;
        return -12;
    }
    cell_save_current(frame);
    if (!vmm_clone_cow(&child_domain->as, &parent->as, 0)) {
        child_thread->state = THREAD_UNUSED;
        child_domain->used = false;
        return -12;
    }
    (void)vma_clone(&child_domain->vmas, &parent->vmas);
    copy_fd_table(child_domain, parent);
    child_domain->parent_id = parent->id;
    child_thread->state = THREAD_RUNNABLE;
    child_thread->tf = current_thread->tf;
    child_thread->tf.x[0] = 0;
    child_thread->tpidr_el0 = current_thread->tpidr_el0;
    current_thread->tf.x[0] = (uint64_t)child_domain->id;
    return child_domain->id;
}

static struct domain *find_waitable_child(int parent_id, int pid) {
    for (size_t i = 0; i < MAX_DOMAINS; ++i) {
        if (domains[i].used && domains[i].zombie && domains[i].parent_id == parent_id &&
            (pid <= 0 || domains[i].id == pid)) {
            return &domains[i];
        }
    }
    return NULL;
}

static bool has_child(int parent_id, int pid) {
    for (size_t i = 0; i < MAX_DOMAINS; ++i) {
        if (domains[i].used && domains[i].parent_id == parent_id &&
            (pid <= 0 || domains[i].id == pid)) {
            return true;
        }
    }
    return false;
}

int cell_wait4(int pid, uint64_t status_addr, struct trap_frame *frame) {
    struct domain *domain = current_domain();
    struct domain *child = find_waitable_child(domain->id, pid);
    if (child == NULL) {
        if (!has_child(domain->id, pid)) {
            return -10;
        }
        cell_save_current(frame);
        current_thread->state = THREAD_BLOCKED;
        current_thread->wait_target = pid;
        cell_schedule(frame);
        return CELL_SWITCHED;
    }

    int status = child->exit_status << 8;
    if (status_addr != 0 && !vmm_copy_to_user(&domain->as, status_addr, &status, sizeof(status))) {
        return -14;
    }
    int child_id = child->id;
    struct thread *child_thread = thread_for_domain(child);
    if (child_thread != NULL) {
        child_thread->state = THREAD_UNUSED;
    }
    destroy_domain(child);
    current_thread->wait_target = -1;
    return child_id;
}

int cell_kill(int pid, int signal) {
    struct domain *domain = find_domain(pid);
    if (domain == NULL || domain->zombie) {
        return -3;
    }
    domain->exit_status = 128 + signal;
    domain->zombie = true;
    struct thread *thread = thread_for_domain(domain);
    if (thread != NULL) {
        thread->state = THREAD_ZOMBIE;
    }
    wake_parent_of(domain);
    return 0;
}

bool cell_handle_cow_fault(uint64_t far) {
    struct domain *domain = current_domain();
    return domain != NULL && vmm_handle_cow_fault(&domain->as, far);
}

int64_t cell_fd_write(int fd, uint64_t buf, uint64_t len) {
    struct domain *domain = current_domain();
    if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
        return -9;
    }
    struct open_file *file = domain->fds[fd];
    if (file->type != OPEN_STDOUT) {
        return -22;
    }
    for (uint64_t i = 0; i < len; ++i) {
        char c;
        if (!vmm_copy_from_user(&domain->as, &c, buf + i, 1)) {
            return -14;
        }
        pl011_putc(c);
    }
    return (int64_t)len;
}

int64_t cell_fd_read(int fd, uint64_t buf, uint64_t len) {
    struct domain *domain = current_domain();
    if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
        return -9;
    }
    struct open_file *file = domain->fds[fd];
    if (file->type == OPEN_STDIN) {
        return 0;
    }
    if (file->type != OPEN_RAMFS || file->node.is_dir) {
        return -22;
    }
    uint64_t remaining = file->node.size > file->offset ? file->node.size - file->offset : 0;
    uint64_t n = remaining < len ? remaining : len;
    if (!vmm_copy_to_user(&domain->as,
                          buf,
                          (const uint8_t *)file->node.data + file->offset,
                          (size_t)n)) {
        return -14;
    }
    file->offset += n;
    return (int64_t)n;
}

int64_t cell_fd_lseek(int fd, int64_t off, int whence) {
    struct domain *domain = current_domain();
    if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
        return -9;
    }
    struct open_file *file = domain->fds[fd];
    int64_t base = 0;
    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = (int64_t)file->offset;
    } else if (whence == 2) {
        base = (int64_t)file->node.size;
    } else {
        return -22;
    }
    int64_t next = base + off;
    if (next < 0) {
        return -22;
    }
    file->offset = (uint64_t)next;
    return next;
}

int cell_fd_open_node(const struct ramfs_node *node, uint32_t flags) {
    struct domain *domain = current_domain();
    if (domain == NULL) {
        return -12;
    }
    int fd = -1;
    for (int i = 0; i < MAX_FDS; ++i) {
        if (domain->fds[i] == NULL) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        return -24;
    }
    struct open_file *file = alloc_open_file();
    if (file == NULL) {
        return -12;
    }
    file->type = OPEN_RAMFS;
    file->flags = flags;
    file->node = *node;
    domain->fds[fd] = file;
    return fd;
}

int cell_fd_dup(int oldfd, int minfd) {
    struct domain *domain = current_domain();
    if (domain == NULL || oldfd < 0 || oldfd >= MAX_FDS ||
        minfd < 0 || minfd >= MAX_FDS || domain->fds[oldfd] == NULL) {
        return -9;
    }
    for (int fd = minfd; fd < MAX_FDS; ++fd) {
        if (domain->fds[fd] == NULL) {
            domain->fds[fd] = domain->fds[oldfd];
            retain_open_file(domain->fds[fd]);
            return fd;
        }
    }
    return -24;
}

int cell_fd_close(int fd) {
    struct domain *domain = current_domain();
    if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
        return -9;
    }
    release_open_file(domain->fds[fd]);
    domain->fds[fd] = NULL;
    return 0;
}

bool cell_fd_stat(int fd, struct ramfs_node *out) {
    struct domain *domain = current_domain();
    if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
        return false;
    }
    struct open_file *file = domain->fds[fd];
    if (file->type == OPEN_STDOUT || file->type == OPEN_STDIN) {
        *out = (struct ramfs_node) {.ino = 10, .is_dir = false};
        return true;
    }
    *out = file->node;
    return true;
}

bool cell_fd_next_dirent(int fd, struct ramfs_dirent *out) {
    struct domain *domain = current_domain();
    if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
        return false;
    }
    struct open_file *file = domain->fds[fd];
    if (file->type != OPEN_RAMFS || !file->node.is_dir) {
        return false;
    }
    if (!ramfs_root_dirent((size_t)file->offset, out)) {
        return false;
    }
    ++file->offset;
    return true;
}

void cell_fd_rewind_one_dirent(int fd) {
    struct domain *domain = current_domain();
    if (domain != NULL && fd >= 0 && fd < MAX_FDS &&
        domain->fds[fd] != NULL && domain->fds[fd]->offset > 0) {
        --domain->fds[fd]->offset;
    }
}

uint64_t cell_fd_dir_offset(int fd) {
    struct domain *domain = current_domain();
    if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
        return 0;
    }
    return domain->fds[fd]->offset;
}

static bool access_allowed(const struct vma *vma, enum vmm_access access) {
    switch (access) {
    case VMM_ACCESS_READ:
        return (vma->prot & VMM_USER_READ) != 0;
    case VMM_ACCESS_WRITE:
        return (vma->prot & VMM_USER_WRITE) != 0;
    case VMM_ACCESS_EXEC:
        return (vma->prot & VMM_USER_EXEC) != 0;
    }
    return false;
}

bool cell_handle_translation_fault(uint64_t far, enum vmm_access access) {
    struct domain *domain = current_domain();
    if (domain == NULL) {
        return false;
    }
    uint64_t va = far & ~(uint64_t)(PAGE_SIZE - 1);
    const struct vma *vma = vma_lookup(&domain->vmas, va);
    if (vma == NULL || vma->type != VMA_ANON || !access_allowed(vma, access)) {
        return false;
    }
    return vmm_alloc_page(&domain->as, va, vma->prot);
}

bool cell_ensure_user_range(uint64_t va, size_t len, enum vmm_access access) {
    struct domain *domain = current_domain();
    if (domain == NULL || len == 0) {
        return true;
    }
    uint64_t end = va + len - 1;
    if (end < va) {
        return false;
    }
    uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = end & ~(uint64_t)(PAGE_SIZE - 1);
    for (;;) {
        if (!vmm_is_mapped(&domain->as, page) &&
            !cell_handle_translation_fault(page, access)) {
            return false;
        }
        if (access == VMM_ACCESS_WRITE &&
            !vmm_user_range_accessible(&domain->as, page, 1, VMM_ACCESS_WRITE) &&
            !vmm_handle_cow_fault(&domain->as, page)) {
            return false;
        }
        if (!vmm_user_range_accessible(&domain->as, page, 1, access)) {
            return false;
        }
        if (page == last) {
            return true;
        }
        page += PAGE_SIZE;
    }
}

bool cell_add_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags) {
    struct domain *domain = current_domain();
    return domain != NULL && vma_insert(&domain->vmas, start, end, prot, flags, VMA_ANON);
}

bool cell_remove_vma(uint64_t start, uint64_t end) {
    struct domain *domain = current_domain();
    if (domain == NULL) {
        return false;
    }
    vmm_unmap_range(&domain->as, start, end);
    return vma_remove(&domain->vmas, start, end);
}

bool cell_protect_vma(uint64_t start, uint64_t end, uint32_t prot) {
    struct domain *domain = current_domain();
    if (domain == NULL) {
        return false;
    }
    if (!vma_protect(&domain->vmas, start, end, prot)) {
        return false;
    }
    vmm_protect_range(&domain->as, start, end, prot);
    return true;
}

size_t cell_resident_pages(uint64_t start, uint64_t end) {
    struct domain *domain = current_domain();
    return domain == NULL ? 0 : vmm_mapped_pages_in_range(&domain->as, start, end);
}

int snapshot_create_current(void) {
    struct domain *domain = current_domain();
    struct snapshot *snap = alloc_snapshot();
    if (domain == NULL || snap == NULL) {
        return -12;
    }
    if (!vmm_clone_cow(&snap->as, &domain->as, 0)) {
        snap->used = false;
        return -12;
    }
    (void)vma_clone(&snap->vmas, &domain->vmas);
    return snap->id;
}

int snapshot_spawn(int snap_id, uint64_t entry, uint64_t arg) {
    struct domain *parent = current_domain();
    struct snapshot *snap = find_snapshot(snap_id);
    struct domain *child_domain = alloc_domain();
    if (parent == NULL || snap == NULL || child_domain == NULL) {
        return -12;
    }
    struct thread *child_thread = alloc_thread(child_domain);
    if (child_thread == NULL) {
        child_domain->used = false;
        return -12;
    }
    if (!vmm_clone_cow(&child_domain->as, &snap->as, 0)) {
        child_thread->state = THREAD_UNUSED;
        child_domain->used = false;
        return -12;
    }
    (void)vma_clone(&child_domain->vmas, &snap->vmas);
    copy_fd_table(child_domain, parent);
    child_domain->parent_id = parent->id;
    child_thread->state = THREAD_RUNNABLE;
    child_thread->tf = current_thread->tf;
    child_thread->tf.elr_el1 = entry;
    child_thread->tf.x[0] = arg;
    child_thread->tf.x[1] = (uint64_t)child_domain->id;
    child_thread->tpidr_el0 = current_thread->tpidr_el0;
    return child_domain->id;
}

int snapshot_reap(int pid, uint64_t status_addr, struct trap_frame *frame) {
    return cell_wait4(pid, status_addr, frame);
}
