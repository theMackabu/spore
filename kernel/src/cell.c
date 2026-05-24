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

enum {
    CELL_O_ACCMODE = 3,
    CELL_O_WRONLY = 1,
    CELL_O_RDWR = 2,
    CELL_O_APPEND = 02000,
    CAP_ENFORCE = 1u << 0,
    CAP_EGRESS_ENFORCE = 1u << 1,
    IPPROTO_UDP = 17,
    EPERM = 1,
    EMSGSIZE = 90,
    EAGAIN = 11,
    EFAULT = 14,
    EINVAL = 22,
    ROBUST_LIST_LIMIT = 16,
    FUTEX_OWNER_DIED = 0x40000000u,
};

struct robust_list_head64 {
    uint64_t next;
    int64_t futex_offset;
    uint64_t pending;
};

static bool str_eq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a++ != *b++) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

static bool starts_with(const char *s, const char *prefix) {
    while (*prefix != '\0') {
        if (*s++ != *prefix++) {
            return false;
        }
    }
    return true;
}

static bool parse_dec(const char **cursor, uint32_t max, uint32_t *out) {
    uint32_t value = 0;
    const char *p = *cursor;
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        value = value * 10u + (uint32_t)(*p - '0');
        if (value > max) {
            return false;
        }
        ++p;
    }
    *cursor = p;
    *out = value;
    return true;
}

static bool parse_ipv4_port(const char *s, uint32_t *ip, uint8_t *prefix, uint16_t *port) {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t prefix_value = 32;
    uint32_t p;
    if (!parse_dec(&s, 255, &a) || *s++ != '.' ||
        !parse_dec(&s, 255, &b) || *s++ != '.' ||
        !parse_dec(&s, 255, &c) || *s++ != '.' ||
        !parse_dec(&s, 255, &d)) {
        return false;
    }
    if (*s == '/') {
        ++s;
        if (!parse_dec(&s, 32, &prefix_value)) {
            return false;
        }
    }
    if (*s++ != ':' ||
        !parse_dec(&s, 65535, &p) || *s != '\0') {
        return false;
    }
    *ip = a | (b << 8) | (c << 16) | (d << 24);
    *prefix = (uint8_t)prefix_value;
    *port = (uint16_t)p;
    return true;
}

static uint32_t egress_mask(uint8_t prefix) {
    if (prefix == 0) {
        return 0;
    }
    return 0xffffffffu >> (32u - prefix);
}

static bool egress_match(const struct capability_set *caps,
                         uint8_t proto,
                         uint32_t ip,
                         uint16_t port) {
    if ((caps->flags & CAP_EGRESS_ENFORCE) == 0) {
        return true;
    }
    uint32_t mask = egress_mask(caps->egress_prefix);
    return caps->egress_proto == proto &&
           caps->egress_port == port &&
           ((caps->egress_ip ^ ip) & mask) == 0;
}

static bool caps_subset(const struct capability_set *requested,
                        const struct capability_set *parent) {
    for (size_t i = 0; i < sizeof(requested->syscall_allow) / sizeof(requested->syscall_allow[0]); ++i) {
        if ((requested->syscall_allow[i] & ~parent->syscall_allow[i]) != 0) {
            return false;
        }
    }
    if ((requested->flags & CAP_EGRESS_ENFORCE) != 0) {
        if ((parent->flags & CAP_EGRESS_ENFORCE) == 0) {
            return true;
        }
        uint32_t parent_mask = egress_mask(parent->egress_prefix);
        uint32_t requested_mask = egress_mask(requested->egress_prefix);
        bool cidr_subset = parent->egress_proto == requested->egress_proto &&
                           parent->egress_port == requested->egress_port &&
                           parent->egress_prefix <= requested->egress_prefix &&
                           ((parent->egress_ip ^ requested->egress_ip) & parent_mask) == 0 &&
                           ((requested->egress_ip & requested_mask) == requested->egress_ip ||
                            requested->egress_prefix == 32);
        if (!cidr_subset) {
            return false;
        }
    }
    if ((requested->flags & CAP_EGRESS_ENFORCE) == 0 &&
        (parent->flags & CAP_EGRESS_ENFORCE) != 0) {
        return false;
    }
    if (parent->memory_page_cap != 0 &&
        (requested->memory_page_cap == 0 || requested->memory_page_cap > parent->memory_page_cap)) {
        return false;
    }
    return true;
}

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
            domains[i].refcount = 0;
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
            threads[i].wait_reason = WAIT_NONE;
            threads[i].wait_target = -1;
            ++domain->refcount;
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

static void copy_domain_metadata(struct domain *dst, const struct domain *src) {
    kmemcpy(dst->cwd, src->cwd, sizeof(dst->cwd));
    kmemcpy(dst->fs_root, src->fs_root, sizeof(dst->fs_root));
    dst->caps = src->caps;
    dst->budget = src->budget;
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
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (threads[i].domain == domain) {
            threads[i].state = THREAD_UNUSED;
            threads[i].domain = NULL;
        }
    }
    close_all_fds(domain);
    vmm_destroy(&domain->as);
    domain->used = false;
    domain->zombie = false;
    domain->refcount = 0;
}

static size_t runnable_or_blocked_threads_in_domain(const struct domain *domain) {
    size_t count = 0;
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (threads[i].domain == domain &&
            (threads[i].state == THREAD_RUNNABLE || threads[i].state == THREAD_BLOCKED)) {
            ++count;
        }
    }
    return count;
}

static void release_thread(struct thread *thread) {
    if (thread == NULL || thread->domain == NULL) {
        return;
    }
    struct domain *domain = thread->domain;
    if (domain->refcount > 0) {
        --domain->refcount;
    }
    thread->state = THREAD_UNUSED;
    thread->domain = NULL;
    if (current_thread == thread) {
        current_thread = NULL;
    }
}

static int futex_wake(struct domain *domain, uint64_t uaddr, uint32_t count) {
    int woke = 0;
    for (size_t i = 0; i < MAX_THREADS && (uint32_t)woke < count; ++i) {
        if (threads[i].domain == domain &&
            threads[i].state == THREAD_BLOCKED &&
            threads[i].wait_reason == WAIT_FUTEX &&
            threads[i].futex_addr == uaddr) {
            threads[i].state = THREAD_RUNNABLE;
            threads[i].wait_reason = WAIT_NONE;
            threads[i].futex_addr = 0;
            threads[i].tf.x[0] = 0;
            ++woke;
        }
    }
    return woke;
}

static void robust_wake_entry(struct domain *domain, uint64_t entry, int64_t futex_offset) {
    uint64_t futex_addr = (uint64_t)((int64_t)entry + futex_offset);
    if ((futex_addr & 3u) != 0 ||
        !cell_ensure_user_range(futex_addr, sizeof(uint32_t), VMM_ACCESS_WRITE)) {
        return;
    }
    uint32_t word = 0;
    if (!vmm_copy_from_user(&domain->as, &word, futex_addr, sizeof(word))) {
        return;
    }
    word |= FUTEX_OWNER_DIED;
    (void)vmm_copy_to_user(&domain->as, futex_addr, &word, sizeof(word));
    (void)futex_wake(domain, futex_addr, 1);
}

static void cleanup_robust_list(struct thread *thread) {
    struct domain *domain = thread->domain;
    if (domain == NULL || thread->robust_list == 0 ||
        !cell_ensure_user_range(thread->robust_list, sizeof(struct robust_list_head64), VMM_ACCESS_READ)) {
        return;
    }
    struct robust_list_head64 head;
    if (!vmm_copy_from_user(&domain->as, &head, thread->robust_list, sizeof(head))) {
        return;
    }
    if (head.pending != 0) {
        robust_wake_entry(domain, head.pending, head.futex_offset);
    }
    uint64_t node = head.next;
    for (size_t i = 0; i < ROBUST_LIST_LIMIT && node != 0 && node != thread->robust_list; ++i) {
        uint64_t next = 0;
        if (!cell_ensure_user_range(node, sizeof(next), VMM_ACCESS_READ) ||
            !vmm_copy_from_user(&domain->as, &next, node, sizeof(next))) {
            break;
        }
        robust_wake_entry(domain, node, head.futex_offset);
        node = next;
    }
}

static void wake_parent_of(struct domain *child) {
    struct domain *parent_domain = find_domain(child->parent_id);
    struct thread *parent = parent_domain == NULL ? NULL : thread_for_domain(parent_domain);
    if (parent != NULL && parent->state == THREAD_BLOCKED &&
        parent->wait_reason == WAIT_CHILD &&
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
        parent->wait_reason = WAIT_NONE;
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
    thread->tf.spsr_el1 = 0x340;
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

const char *cell_current_cwd(void) {
    struct domain *domain = current_domain();
    return domain == NULL ? "/" : domain->cwd;
}

bool cell_set_cwd(const char *path) {
    struct domain *domain = current_domain();
    if (domain == NULL || path == NULL || path[0] != '/') {
        return false;
    }
    size_t len = kstrlen(path);
    if (len >= sizeof(domain->cwd)) {
        return false;
    }
    kmemcpy(domain->cwd, path, len + 1);
    return true;
}

const char *cell_current_fs_root(void) {
    struct domain *domain = current_domain();
    return domain == NULL ? "/" : domain->fs_root;
}

static void cap_allow(struct capability_set *caps, uint64_t nr) {
    if (nr < 512) {
        caps->syscall_allow[nr / 64] |= 1ull << (nr % 64);
    }
}

static void cap_allow_common(struct capability_set *caps) {
    static const uint16_t common[] = {
        17, 23, 24, 25, 29, 57, 63, 64, 65, 66, 80, 93, 94, 96, 98, 99,
        101, 113, 115, 123, 124, 134, 135, 160, 172, 173, 174, 175,
        176, 177, 178, 179, 198, 200, 203, 204, 206, 207, 208,
        214, 215, 216, 220, 221, 222, 226, 233,
        260, 261, 278,
    };
    for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); ++i) {
        cap_allow(caps, common[i]);
    }
}

static void cap_allow_files(struct capability_set *caps) {
    static const uint16_t files[] = {34, 35, 38, 46, 49, 50, 56, 61, 62, 78, 79, 82, 276};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        cap_allow(caps, files[i]);
    }
}

bool cell_syscall_allowed(uint64_t nr) {
    struct domain *domain = current_domain();
    if (domain == NULL || (domain->caps.flags & CAP_ENFORCE) == 0) {
        return true;
    }
    if (nr >= 512) {
        return false;
    }
    return (domain->caps.syscall_allow[nr / 64] & (1ull << (nr % 64))) != 0;
}

bool cell_egress_allowed(uint8_t proto, uint32_t ip, uint16_t port) {
    struct domain *domain = current_domain();
    if (domain == NULL || (domain->caps.flags & CAP_EGRESS_ENFORCE) == 0) {
        return true;
    }
    bool allowed = egress_match(&domain->caps, proto, ip, port);
    if (!allowed) {
        kprintf("[spore] egress denied domain=%d proto=%u dst=%x:%u\n",
                domain->id,
                (unsigned)proto,
                (unsigned)ip,
                (unsigned)port);
    }
    return allowed;
}

bool cell_mmap_allowed(uint64_t pages) {
    struct domain *domain = current_domain();
    return domain == NULL || domain->caps.memory_page_cap == 0 ||
           pages <= domain->caps.memory_page_cap;
}

int cell_apply_policy(const char *manifest) {
    struct domain *domain = current_domain();
    if (domain == NULL) {
        return -3;
    }
    if (str_eq(manifest, "bad-manifest")) {
        return -1;
    }

    struct capability_set caps = {0};
    cap_allow_common(&caps);
    caps.flags = CAP_ENFORCE;
    domain->fs_root[0] = '/';
    domain->fs_root[1] = '\0';

    if (str_eq(manifest, "compute-only")) {
        domain->budget.max_ticks = 20;
        domain->budget.remaining_ticks = 20;
    } else if (str_eq(manifest, "fs:/tmp")) {
        cap_allow_files(&caps);
        domain->fs_root[0] = '/';
        domain->fs_root[1] = 't';
        domain->fs_root[2] = 'm';
        domain->fs_root[3] = 'p';
        domain->fs_root[4] = '\0';
    } else if (str_eq(manifest, "mem:1")) {
        caps.memory_page_cap = 1;
    } else if (str_eq(manifest, "net:none")) {
        caps.flags |= CAP_EGRESS_ENFORCE;
        caps.egress_prefix = 32;
    } else if (starts_with(manifest, "net:udp:")) {
        uint32_t ip = 0;
        uint8_t prefix = 32;
        uint16_t port = 0;
        if (!parse_ipv4_port(manifest + 8, &ip, &prefix, &port)) {
            return -2;
        }
        caps.flags |= CAP_EGRESS_ENFORCE;
        caps.egress_proto = IPPROTO_UDP;
        caps.egress_prefix = prefix;
        caps.egress_ip = ip & egress_mask(prefix);
        caps.egress_port = port;
    } else {
        return -2;
    }
    if ((domain->caps.flags & CAP_ENFORCE) != 0 && !caps_subset(&caps, &domain->caps)) {
        return -1;
    }
    domain->caps = caps;
    kprintf("[spore] policy applied: %s\n", manifest);
    return 0;
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
    size_t start = current_thread == NULL ? 0 : (size_t)(current_thread - threads + 1);
    for (;;) {
        if (current_thread != NULL && current_thread->state == THREAD_RUNNABLE) {
            for (size_t n = 0; n < MAX_THREADS; ++n) {
                struct thread *candidate = &threads[(start + n) % MAX_THREADS];
                if (candidate->state == THREAD_RUNNABLE) {
                    current_thread = candidate;
                    restore_thread(candidate, frame, old_domain);
                    return;
                }
            }
        }
        for (size_t n = 0; n < MAX_THREADS; ++n) {
            struct thread *candidate = &threads[(start + n) % MAX_THREADS];
            if (candidate->state == THREAD_RUNNABLE) {
                current_thread = candidate;
                restore_thread(candidate, frame, old_domain);
                return;
            }
        }

        bool has_blocked = false;
        for (size_t i = 0; i < MAX_THREADS; ++i) {
            if (threads[i].state == THREAD_BLOCKED) {
                has_blocked = true;
                break;
            }
        }
        if (!has_blocked) {
            break;
        }
        __asm__ volatile(
            "msr daifclr, #2\n"
            "wfi\n"
            "msr daifset, #2\n"
            :
            :
            : "memory");
    }
    kprintf("[kernel] no runnable threads\n");
    poweroff();
    for (;;) {
        __asm__ volatile("wfe");
    }
}

void cell_exit_thread_current(int status, struct trap_frame *frame) {
    if (current_thread == NULL) {
        return;
    }
    struct domain *domain = current_thread->domain;
    cleanup_robust_list(current_thread);
    if (current_thread->clear_child_tid != 0) {
        uint32_t zero = 0;
        (void)vmm_copy_to_user(&domain->as, current_thread->clear_child_tid, &zero, sizeof(zero));
        (void)futex_wake(domain, current_thread->clear_child_tid, 1);
    }
    if (runnable_or_blocked_threads_in_domain(domain) <= 1) {
        domain->exit_status = status;
        domain->zombie = true;
        current_thread->state = THREAD_ZOMBIE;
        wake_parent_of(domain);
        cell_schedule(frame);
        return;
    }
    kprintf("[kernel] exit(%d) tid=%d\n", status, current_thread->tid);
    release_thread(current_thread);
    cell_schedule(frame);
}

void cell_exit_group_current(int status, struct trap_frame *frame) {
    if (current_thread == NULL) {
        return;
    }
    struct domain *domain = current_thread->domain;
    domain->exit_status = status;
    domain->zombie = true;
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (&threads[i] != current_thread && threads[i].domain == domain) {
            threads[i].state = THREAD_UNUSED;
            threads[i].domain = NULL;
            if (domain->refcount > 0) {
                --domain->refcount;
            }
        }
    }
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
    copy_domain_metadata(child_domain, parent);
    copy_fd_table(child_domain, parent);
    child_domain->parent_id = parent->id;
    child_thread->state = THREAD_RUNNABLE;
    child_thread->tf = current_thread->tf;
    child_thread->tf.x[0] = 0;
    child_thread->tpidr_el0 = current_thread->tpidr_el0;
    current_thread->tf.x[0] = (uint64_t)child_domain->id;
    return child_domain->id;
}

int cell_clone_thread_current(struct trap_frame *frame,
                              uint64_t flags,
                              uint64_t newsp,
                              uint64_t parent_tid,
                              uint64_t tls,
                              uint64_t child_tid) {
    struct domain *domain = current_domain();
    if (domain == NULL || current_thread == NULL || newsp == 0) {
        return -22;
    }
    struct thread *thread = alloc_thread(domain);
    if (thread == NULL) {
        return -12;
    }
    cell_save_current(frame);
    thread->state = THREAD_RUNNABLE;
    thread->tf = current_thread->tf;
    thread->tf.x[0] = 0;
    thread->tf.sp_el0 = newsp;
    thread->tpidr_el0 = ((flags & 0x00080000ull) != 0) ? tls : current_thread->tpidr_el0;
    thread->clear_child_tid = ((flags & 0x00200000ull) != 0) ? child_tid : 0;
    if ((flags & 0x00100000ull) != 0 && parent_tid != 0) {
        uint32_t tid = (uint32_t)thread->tid;
        (void)vmm_copy_to_user(&domain->as, parent_tid, &tid, sizeof(tid));
    }
    if ((flags & 0x01000000ull) != 0 && child_tid != 0) {
        uint32_t tid = (uint32_t)thread->tid;
        (void)vmm_copy_to_user(&domain->as, child_tid, &tid, sizeof(tid));
    }
    current_thread->tf.x[0] = (uint64_t)thread->tid;
    return thread->tid;
}

int cell_set_tid_address_current(uint64_t clear_child_tid) {
    if (current_thread == NULL) {
        return 0;
    }
    current_thread->clear_child_tid = clear_child_tid;
    return current_thread->tid;
}

int cell_set_robust_list_current(uint64_t robust_list) {
    if (current_thread == NULL) {
        return -22;
    }
    current_thread->robust_list = robust_list;
    return 0;
}

int cell_futex_wait_current(uint64_t uaddr, uint32_t expected, struct trap_frame *frame) {
    struct domain *domain = current_domain();
    if (domain == NULL || current_thread == NULL || (uaddr & 3u) != 0) {
        return -EINVAL;
    }
    if (!cell_ensure_user_range(uaddr, sizeof(uint32_t), VMM_ACCESS_READ)) {
        return -EFAULT;
    }
    uint32_t actual = 0;
    if (!vmm_copy_from_user(&domain->as, &actual, uaddr, sizeof(actual))) {
        return -EFAULT;
    }
    if (actual != expected) {
        return -EAGAIN;
    }
    cell_save_current(frame);
    current_thread->state = THREAD_BLOCKED;
    current_thread->wait_reason = WAIT_FUTEX;
    current_thread->futex_addr = uaddr;
    cell_schedule(frame);
    return CELL_SWITCHED;
}

int cell_futex_wake_current(uint64_t uaddr, uint32_t count) {
    struct domain *domain = current_domain();
    if (domain == NULL || (uaddr & 3u) != 0) {
        return -EINVAL;
    }
    if (count == 0) {
        return 0;
    }
    return futex_wake(domain, uaddr, count);
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
        current_thread->wait_reason = WAIT_CHILD;
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
    current_thread->wait_reason = WAIT_NONE;
    return child_id;
}

int cell_kill(int pid, int signal) {
    struct domain *domain = find_domain(pid);
    if (domain == NULL || domain->zombie) {
        return -3;
    }
    domain->exit_status = 128 + signal;
    domain->zombie = true;
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (threads[i].domain == domain) {
            threads[i].state = THREAD_ZOMBIE;
        }
    }
    wake_parent_of(domain);
    return 0;
}

bool cell_exec_replace(struct user_address_space *as,
                       struct vma_list *vmas,
                       uint64_t entry,
                       uint64_t sp,
                       struct trap_frame *frame) {
    struct domain *domain = current_domain();
    if (domain == NULL || current_thread == NULL) {
        return false;
    }

    struct user_address_space old_as = domain->as;
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (&threads[i] != current_thread && threads[i].domain == domain) {
            threads[i].state = THREAD_UNUSED;
            threads[i].domain = NULL;
            if (domain->refcount > 0) {
                --domain->refcount;
            }
        }
    }
    domain->as = *as;
    domain->vmas = *vmas;
    vmm_install_user(&domain->as);
    vmm_destroy(&old_as);

    kmemset(frame, 0, sizeof(*frame));
    frame->elr_el1 = entry;
    frame->sp_el0 = sp;
    frame->spsr_el1 = 0x340;
    current_thread->tf = *frame;
    current_thread->tpidr_el0 = 0;
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(0ull));
    return true;
}

int cell_set_budget(int domain_id, uint64_t ticks) {
    struct domain *domain = domain_id == 0 ? current_domain() : find_domain(domain_id);
    if (domain == NULL) {
        return -3;
    }
    domain->budget.max_ticks = ticks;
    domain->budget.remaining_ticks = ticks;
    if (ticks != 0) {
        kprintf("[spore] domain %d CPU budget set to %u ticks\n",
                domain->id,
                (unsigned)ticks);
    }
    return 0;
}

void cell_timer_tick(struct trap_frame *frame, bool from_lower_el) {
    struct domain *domain = current_domain();
    if (domain == NULL) {
        return;
    }
    if (domain->budget.max_ticks != 0 && domain->budget.remaining_ticks != 0) {
        --domain->budget.remaining_ticks;
        if (domain->budget.remaining_ticks == 0) {
            kprintf("[spore] domain %d exceeded CPU budget -> killed\n", domain->id);
            if (from_lower_el) {
                cell_exit_group_current(137, frame);
            } else {
                domain->zombie = true;
                domain->exit_status = 137;
            }
            return;
        }
    }
    if (from_lower_el) {
        cell_schedule(frame);
    }
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
        if (file->type != OPEN_RAMFS ||
            ((file->flags & CELL_O_ACCMODE) != CELL_O_WRONLY &&
             (file->flags & CELL_O_ACCMODE) != CELL_O_RDWR)) {
            return -22;
        }
        if ((file->flags & CELL_O_APPEND) != 0) {
            struct ramfs_node fresh;
            if (ramfs_refresh_node(file->node.fs, file->node.index, &fresh)) {
                file->offset = fresh.size;
            }
        }
        uint8_t tmp[128];
        uint64_t done = 0;
        while (done < len) {
            uint64_t chunk = len - done;
            if (chunk > sizeof(tmp)) {
                chunk = sizeof(tmp);
            }
            if (!vmm_copy_from_user(&domain->as, tmp, buf + done, (size_t)chunk)) {
                return -14;
            }
            int64_t wrote = ramfs_write(file->node.fs, file->node.index, file->offset, tmp, chunk);
            if (wrote < 0) {
                return -28;
            }
            file->offset += (uint64_t)wrote;
            done += (uint64_t)wrote;
        }
        return (int64_t)done;
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

static int64_t read_stdin_to_user(struct domain *domain, uint64_t buf, uint64_t len) {
    uint64_t n = 0;
    while (n < len) {
        char c;
        if (!pl011_getc(&c)) {
            break;
        }
        if (!vmm_copy_to_user(&domain->as, buf + n, &c, 1)) {
            return -14;
        }
        ++n;
        if (c == '\n' || c == '\r') {
            break;
        }
    }
    return (int64_t)n;
}

int64_t cell_fd_read(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame) {
    struct domain *domain = current_domain();
    if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) {
        return -9;
    }
    struct open_file *file = domain->fds[fd];
    if (file->type == OPEN_STDIN) {
        if (len == 0) {
            return 0;
        }
        int64_t n = read_stdin_to_user(domain, buf, len);
        if (n != 0) {
            return n;
        }
        if (frame == NULL) {
            return 0;
        }
        cell_save_current(frame);
        current_thread->state = THREAD_BLOCKED;
        current_thread->wait_reason = WAIT_STDIN;
        current_thread->stdin_buf = buf;
        current_thread->stdin_len = len;
        cell_schedule(frame);
        return CELL_SWITCHED;
    }
    if (file->type != OPEN_RAMFS || file->node.is_dir) {
        return -22;
    }
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = len - done;
        if (chunk > sizeof(tmp)) {
            chunk = sizeof(tmp);
        }
        uint64_t got = ramfs_read(file->node.fs, file->node.index, file->offset, tmp, chunk);
        if (got == 0) {
            break;
        }
        if (!vmm_copy_to_user(&domain->as, buf + done, tmp, (size_t)got)) {
            return -14;
        }
        file->offset += got;
        done += got;
    }
    return (int64_t)done;
}

void cell_wake_stdin(void) {
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        struct thread *thread = &threads[i];
        if (thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_STDIN) {
            continue;
        }
        int64_t n = read_stdin_to_user(thread->domain, thread->stdin_buf, thread->stdin_len);
        if (n <= 0) {
            continue;
        }
        thread->tf.x[0] = (uint64_t)n;
        thread->state = THREAD_RUNNABLE;
        thread->wait_reason = WAIT_NONE;
        thread->stdin_buf = 0;
        thread->stdin_len = 0;
    }
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
        struct ramfs_node fresh;
        base = ramfs_refresh_node(file->node.fs, file->node.index, &fresh) ? (int64_t)fresh.size : 0;
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
    if ((flags & CELL_O_APPEND) != 0) {
        file->offset = node->size;
    }
    domain->fds[fd] = file;
    return fd;
}

int cell_fd_socket_udp(void) {
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
    file->type = OPEN_SOCKET;
    file->udp_local_port = (uint16_t)(40000 + fd);
    domain->fds[fd] = file;
    return fd;
}

static struct open_file *udp_socket_for_fd(struct domain *domain, int fd) {
    if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL ||
        domain->fds[fd]->type != OPEN_SOCKET) {
        return NULL;
    }
    return domain->fds[fd];
}

bool cell_fd_udp_bind(int fd, uint16_t port) {
    struct open_file *file = udp_socket_for_fd(current_domain(), fd);
    if (file == NULL) {
        return false;
    }
    file->udp_local_port = port;
    return true;
}

bool cell_fd_udp_connect(int fd, uint32_t ip, uint16_t port) {
    struct open_file *file = udp_socket_for_fd(current_domain(), fd);
    if (file == NULL) {
        return false;
    }
    file->udp_remote_ip = ip;
    file->udp_remote_port = port;
    file->udp_connected = true;
    return true;
}

int64_t cell_fd_udp_send(int fd, uint32_t ip, uint16_t port, uint64_t buf, uint64_t len) {
    struct domain *domain = current_domain();
    struct open_file *file = udp_socket_for_fd(domain, fd);
    if (file == NULL) {
        return -9;
    }
    uint32_t effective_ip = ip;
    uint16_t effective_port = port;
    if (effective_ip == 0 && effective_port == 0 && file->udp_connected) {
        effective_ip = file->udp_remote_ip;
        effective_port = file->udp_remote_port;
    }
    if (effective_ip == 0 || effective_port == 0) {
        return -EINVAL;
    }
    if (!cell_egress_allowed(IPPROTO_UDP, effective_ip, effective_port)) {
        return -EPERM;
    }
    if (!file->udp_connected) {
        file->udp_remote_ip = effective_ip;
        file->udp_remote_port = effective_port;
        file->udp_connected = true;
    }
    if (len > sizeof(file->udp_rx)) {
        return -EMSGSIZE;
    }
    if (!vmm_copy_from_user(&domain->as, file->udp_rx, buf, (size_t)len)) {
        return -EFAULT;
    }
    file->udp_rx_len = len;
    kprintf("[spore] udp send fd=%d dst=%x:%u len=%u\n",
            fd,
            (unsigned)effective_ip,
            (unsigned)effective_port,
            (unsigned)len);
    return (int64_t)len;
}

int64_t cell_fd_udp_recv(int fd, uint64_t buf, uint64_t len) {
    struct domain *domain = current_domain();
    struct open_file *file = udp_socket_for_fd(domain, fd);
    if (file == NULL) {
        return -9;
    }
    if (file->udp_rx_len == 0) {
        return -EAGAIN;
    }
    uint64_t n = file->udp_rx_len < len ? file->udp_rx_len : len;
    if (!vmm_copy_to_user(&domain->as, buf, file->udp_rx, (size_t)n)) {
        return -EFAULT;
    }
    file->udp_rx_len = 0;
    return (int64_t)n;
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
    return ramfs_refresh_node(file->node.fs, file->node.index, out);
}

bool cell_fd_is_dir(int fd) {
    struct ramfs_node node;
    return cell_fd_stat(fd, &node) && node.is_dir;
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
    if (!ramfs_dirent(file->node.fs, file->node.index, (size_t)file->offset, out)) {
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

int snapshot_spawn(int snap_id, uint64_t entry, uint64_t arg, struct trap_frame *frame) {
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
    copy_domain_metadata(child_domain, parent);
    copy_fd_table(child_domain, parent);
    child_domain->parent_id = parent->id;
    child_thread->state = THREAD_RUNNABLE;
    cell_save_current(frame);
    child_thread->tf = *frame;
    child_thread->tf.elr_el1 = entry;
    child_thread->tf.x[0] = arg;
    child_thread->tf.x[1] = (uint64_t)child_domain->id;
    child_thread->tf.spsr_el1 &= ~(1ull << 7);
    child_thread->tpidr_el0 = current_thread->tpidr_el0;
    return child_domain->id;
}

int snapshot_reap(int pid, uint64_t status_addr, struct trap_frame *frame) {
    return cell_wait4(pid, status_addr, frame);
}
