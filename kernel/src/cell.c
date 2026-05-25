#include "cell.h"

#include "exec/stack.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "net.h"
#include "pl011.h"
#include "vfs.h"
#include "virtio_blk.h"
#include "virtio_net.h"

static struct domain domains[MAX_DOMAINS];
static struct thread threads[MAX_THREADS];
static struct snapshot snapshots[MAX_SNAPSHOTS];
static struct open_file open_files[MAX_OPEN_FILES];
static struct thread *current_thread;
static int next_domain_id = 1;
static int next_thread_id = 1;
static int next_snapshot_id;
static uint64_t device_rng_state = 0x9e3779b97f4a7c15ull;
static uint64_t scheduler_ticks;
static uint64_t scheduler_idle_ticks;
static uint64_t boot_epoch_sec;
static uint64_t proc_cache_tick = UINT64_MAX;
static size_t proc_cache_rss[MAX_DOMAINS];
static uint32_t tty_lflag = 0000002 | 0000010;
static char tty_line[256];
static size_t tty_line_len;
static size_t tty_line_cursor;
static char tty_ready[512];
static size_t tty_ready_head;
static size_t tty_ready_len;
static char tty_output_line[256];
static size_t tty_output_line_len;
static char tty_prompt[256];
static size_t tty_prompt_len;
static bool tty_prompt_active;
static volatile bool scheduler_waiting_for_interrupt;

static size_t domain_resident_pages(const struct domain *domain);
static void wake_poll_waiters(void);
static void wake_sleep_waiters(void);

enum {
  CELL_O_ACCMODE = 3,
  CELL_O_WRONLY = 1,
  CELL_O_RDWR = 2,
  CELL_O_APPEND = 02000,
  CAP_ENFORCE = 1u << 0,
  CAP_EGRESS_ENFORCE = 1u << 1,
  IPPROTO_UDP = 17,
  IPPROTO_ICMP = 1,
  EPERM = 1,
  EMSGSIZE = 90,
  EAGAIN = 11,
  EFAULT = 14,
  EINVAL = 22,
  EIO = 5,
  ECHILD = 10,
  ESRCH = 3,
  WNOHANG = 1,
  SIGINT = 2,
  SIGKILL = 9,
  SIGSEGV = 11,
  SIGTERM = 15,
  TTY_ICANON = 0000002,
  TTY_ECHO = 0000010,
  ROBUST_LIST_LIMIT = 16,
  FUTEX_OWNER_DIED = 0x40000000u,
};

struct robust_list_head64 {
  uint64_t next;
  int64_t futex_offset;
  uint64_t pending;
};

struct pollfd64 {
  int32_t fd;
  int16_t events;
  int16_t revents;
};

static bool str_eq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

static bool starts_with(const char *s, const char *prefix) {
  while (*prefix != '\0') {
    if (*s++ != *prefix++) { return false; }
  }
  return true;
}

static void copy_cstr(char *dst, size_t cap, const char *src) {
  if (cap == 0) { return; }
  size_t i = 0;
  if (src != NULL) {
    for (; i + 1 < cap && src[i] != '\0'; ++i) {
      dst[i] = src[i];
    }
  }
  dst[i] = '\0';
}

static const char *base_name(const char *path) {
  const char *base = path == NULL ? "" : path;
  for (const char *p = base; *p != '\0'; ++p) {
    if (*p == '/' && p[1] != '\0') { base = p + 1; }
  }
  return base;
}

static void set_domain_identity(struct domain *domain, const char *path, const char *const argv[], uint64_t argc) {
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

static bool parse_dec(const char **cursor, uint32_t max, uint32_t *out) {
  uint32_t value = 0;
  const char *p = *cursor;
  if (*p < '0' || *p > '9') { return false; }
  while (*p >= '0' && *p <= '9') {
    value = value * 10u + (uint32_t)(*p - '0');
    if (value > max) { return false; }
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
  if (!parse_dec(&s, 255, &a) || *s++ != '.' || !parse_dec(&s, 255, &b) || *s++ != '.' || !parse_dec(&s, 255, &c) ||
      *s++ != '.' || !parse_dec(&s, 255, &d)) {
    return false;
  }
  if (*s == '/') {
    ++s;
    if (!parse_dec(&s, 32, &prefix_value)) { return false; }
  }
  if (*s++ != ':' || !parse_dec(&s, 65535, &p) || *s != '\0') { return false; }
  *ip = a | (b << 8) | (c << 16) | (d << 24);
  *prefix = (uint8_t)prefix_value;
  *port = (uint16_t)p;
  return true;
}

static uint32_t egress_mask(uint8_t prefix) {
  if (prefix == 0) { return 0; }
  return 0xffffffffu >> (32u - prefix);
}

static bool egress_match(const struct capability_set *caps, uint8_t proto, uint32_t ip, uint16_t port) {
  if ((caps->flags & CAP_EGRESS_ENFORCE) == 0) { return true; }
  uint32_t mask = egress_mask(caps->egress_prefix);
  return caps->egress_proto == proto && caps->egress_port == port && ((caps->egress_ip ^ ip) & mask) == 0;
}

static bool caps_subset(const struct capability_set *requested, const struct capability_set *parent) {
  for (size_t i = 0; i < sizeof(requested->syscall_allow) / sizeof(requested->syscall_allow[0]); ++i) {
    if ((requested->syscall_allow[i] & ~parent->syscall_allow[i]) != 0) { return false; }
  }
  if ((requested->flags & CAP_EGRESS_ENFORCE) != 0) {
    if ((parent->flags & CAP_EGRESS_ENFORCE) == 0) { return true; }
    uint32_t parent_mask = egress_mask(parent->egress_prefix);
    uint32_t requested_mask = egress_mask(requested->egress_prefix);
    bool cidr_subset =
      parent->egress_proto == requested->egress_proto && parent->egress_port == requested->egress_port &&
      parent->egress_prefix <= requested->egress_prefix &&
      ((parent->egress_ip ^ requested->egress_ip) & parent_mask) == 0 &&
      ((requested->egress_ip & requested_mask) == requested->egress_ip || requested->egress_prefix == 32);
    if (!cidr_subset) { return false; }
  }
  if ((requested->flags & CAP_EGRESS_ENFORCE) == 0 && (parent->flags & CAP_EGRESS_ENFORCE) != 0) { return false; }
  if (parent->memory_page_cap != 0 &&
      (requested->memory_page_cap == 0 || requested->memory_page_cap > parent->memory_page_cap)) {
    return false;
  }
  return true;
}

static void poweroff(void) {
  __asm__ volatile("mov x0, #0x0008\n"
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
    if (domains[i].used && domains[i].id == id) { return &domains[i]; }
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
      domains[i].uid = 0;
      domains[i].euid = 0;
      domains[i].gid = 0;
      domains[i].egid = 0;
      domains[i].start_ticks = scheduler_ticks;
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
  if (file != NULL) { ++file->refcount; }
}

static void release_open_file(struct open_file *file) {
  if (file == NULL || file->refcount == 0) { return; }
  --file->refcount;
  if (file->refcount == 0) { file->used = false; }
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
  dst->start_ticks = scheduler_ticks;
  dst->cpu_ticks = 0;
}

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

static struct thread *thread_for_domain(struct domain *domain) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].state != THREAD_UNUSED && threads[i].domain == domain) { return &threads[i]; }
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
    if (threads[i].domain == domain && (threads[i].state == THREAD_RUNNABLE || threads[i].state == THREAD_BLOCKED)) {
      ++count;
    }
  }
  return count;
}

static void release_thread(struct thread *thread) {
  if (thread == NULL || thread->domain == NULL) { return; }
  struct domain *domain = thread->domain;
  if (domain->refcount > 0) { --domain->refcount; }
  thread->state = THREAD_UNUSED;
  thread->domain = NULL;
  if (current_thread == thread) { current_thread = NULL; }
}

static int futex_wake(struct domain *domain, uint64_t uaddr, uint32_t count) {
  int woke = 0;
  for (size_t i = 0; i < MAX_THREADS && (uint32_t)woke < count; ++i) {
    if (threads[i].domain == domain && threads[i].state == THREAD_BLOCKED && threads[i].wait_reason == WAIT_FUTEX &&
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
  if ((futex_addr & 3u) != 0 || !cell_ensure_user_range(futex_addr, sizeof(uint32_t), VMM_ACCESS_WRITE)) { return; }
  uint32_t word = 0;
  if (!vmm_copy_from_user(&domain->as, &word, futex_addr, sizeof(word))) { return; }
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
  if (!vmm_copy_from_user(&domain->as, &head, thread->robust_list, sizeof(head))) { return; }
  if (head.pending != 0) { robust_wake_entry(domain, head.pending, head.futex_offset); }
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
  if (parent != NULL && parent->state == THREAD_BLOCKED && parent->wait_reason == WAIT_CHILD &&
      (parent->wait_target < 0 || parent->wait_target == child->id)) {
    int status = child->exit_status << 8;
    if (child->term_signal != 0) { status = child->term_signal; }
    uint64_t status_addr = parent->tf.x[1];
    if (status_addr != 0) { (void)vmm_copy_to_user(&parent_domain->as, status_addr, &status, sizeof(status)); }
    parent->tf.x[0] = (uint64_t)child->id;
    struct thread *child_thread = thread_for_domain(child);
    if (child_thread != NULL) { child_thread->state = THREAD_UNUSED; }
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
  scheduler_ticks = 0;
  scheduler_idle_ticks = 0;
  scheduler_waiting_for_interrupt = false;
  // v2 Phase A object model: domains own isolation/policy state, threads own
  // EL0 execution state. The kernel remains run-to-completion on one core, so
  // these tables intentionally have no locks until a later SMP/preemptive goal.
}

bool cell_create_init(struct user_address_space *as, uint64_t entry, uint64_t sp) {
  struct domain *domain = alloc_domain();
  if (domain == NULL) { return false; }
  struct thread *thread = alloc_thread(domain);
  if (thread == NULL) {
    domain->used = false;
    return false;
  }
  domain->parent_id = 0;
  static const char *init_argv[] = {"/init"};
  set_domain_identity(domain, "/init", init_argv, 1);
  domain->as = *as;
  domain->as.asid = 0;
  vma_list_init(&domain->vmas);
  if (!vma_insert(&domain->vmas, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, VMM_USER_READ | VMM_USER_WRITE, 0,
                  VMA_ANON)) {
    thread->state = THREAD_UNUSED;
    domain->used = false;
    return false;
  }
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

uint32_t cell_current_uid(void) {
  struct domain *domain = current_domain();
  return domain == NULL ? 0 : domain->uid;
}

uint32_t cell_current_euid(void) {
  struct domain *domain = current_domain();
  return domain == NULL ? 0 : domain->euid;
}

uint32_t cell_current_gid(void) {
  struct domain *domain = current_domain();
  return domain == NULL ? 0 : domain->gid;
}

uint32_t cell_current_egid(void) {
  struct domain *domain = current_domain();
  return domain == NULL ? 0 : domain->egid;
}

int cell_setuid_current(uint32_t uid) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return -1; }
  if (domain->euid != 0 && uid != domain->uid) { return -1; }
  domain->uid = uid;
  domain->euid = uid;
  return 0;
}

int cell_setgid_current(uint32_t gid) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return -1; }
  if (domain->euid != 0 && gid != domain->gid) { return -1; }
  domain->gid = gid;
  domain->egid = gid;
  return 0;
}

void cell_apply_exec_creds(uint32_t mode, uint32_t uid, uint32_t gid) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return; }
  if ((mode & 04000u) != 0) { domain->euid = uid; }
  if ((mode & 02000u) != 0) { domain->egid = gid; }
}

const char *cell_current_cwd(void) {
  struct domain *domain = current_domain();
  return domain == NULL ? "/" : domain->cwd;
}

bool cell_set_cwd(const char *path) {
  struct domain *domain = current_domain();
  if (domain == NULL || path == NULL || path[0] != '/') { return false; }
  size_t len = kstrlen(path);
  if (len >= sizeof(domain->cwd)) { return false; }
  kmemcpy(domain->cwd, path, len + 1);
  return true;
}

const char *cell_current_fs_root(void) {
  struct domain *domain = current_domain();
  return domain == NULL ? "/" : domain->fs_root;
}

const char *cell_current_chroot(void) {
  struct domain *domain = current_domain();
  return domain == NULL ? "/" : domain->chroot;
}

bool cell_set_chroot(const char *path) {
  struct domain *domain = current_domain();
  if (domain == NULL || path == NULL || path[0] != '/') { return false; }
  size_t len = kstrlen(path);
  if (len >= sizeof(domain->chroot)) { return false; }
  kmemcpy(domain->chroot, path, len + 1);
  return true;
}

static void cap_allow(struct capability_set *caps, uint64_t nr) {
  if (nr < 512) { caps->syscall_allow[nr / 64] |= 1ull << (nr % 64); }
}

static void cap_allow_common(struct capability_set *caps) {
  static const uint16_t common[] = {
    17,  23,  24,  25,  29,  57,  63,  64,  65,  66,  80,  93,  94,  96,  98,  99,  101, 48,
    113, 115, 123, 124, 134, 135, 144, 146, 160, 172, 173, 174, 175, 176, 177, 178, 179, 198,
    200, 203, 204, 206, 207, 208, 214, 215, 216, 220, 221, 222, 226, 233, 260, 261, 278, 439,
  };
  for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); ++i) {
    cap_allow(caps, common[i]);
  }
}

static void cap_allow_files(struct capability_set *caps) {
  static const uint16_t files[] = {34, 35, 38, 46, 49, 50, 52, 53, 54, 55, 56, 61, 62, 78, 79, 82, 276};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    cap_allow(caps, files[i]);
  }
}

bool cell_syscall_allowed(uint64_t nr) {
  struct domain *domain = current_domain();
  if (domain == NULL || (domain->caps.flags & CAP_ENFORCE) == 0) { return true; }
  if (nr >= 512) { return false; }
  return (domain->caps.syscall_allow[nr / 64] & (1ull << (nr % 64))) != 0;
}

bool cell_egress_allowed(uint8_t proto, uint32_t ip, uint16_t port) {
  struct domain *domain = current_domain();
  if (domain == NULL || (domain->caps.flags & CAP_EGRESS_ENFORCE) == 0) { return true; }
  bool allowed = egress_match(&domain->caps, proto, ip, port);
  if (!allowed) {
    kprintf("[spore] egress denied domain=%d proto=%u dst=%x:%u\n", domain->id, (unsigned)proto, (unsigned)ip,
            (unsigned)port);
  }
  return allowed;
}

bool cell_mmap_allowed(uint64_t pages) {
  struct domain *domain = current_domain();
  return domain == NULL || domain->caps.memory_page_cap == 0 || pages <= domain->caps.memory_page_cap;
}

int cell_apply_policy(const char *manifest) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return -3; }
  if (str_eq(manifest, "bad-manifest")) { return -1; }

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
    if (!parse_ipv4_port(manifest + 8, &ip, &prefix, &port)) { return -2; }
    caps.flags |= CAP_EGRESS_ENFORCE;
    caps.egress_proto = IPPROTO_UDP;
    caps.egress_prefix = prefix;
    caps.egress_ip = ip & egress_mask(prefix);
    caps.egress_port = port;
  } else {
    return -2;
  }
  if ((domain->caps.flags & CAP_ENFORCE) != 0 && !caps_subset(&caps, &domain->caps)) { return -1; }
  domain->caps = caps;
  kprintf("[spore] policy applied: %s\n", manifest);
  return 0;
}

void cell_save_current(const struct trap_frame *frame) {
  if (current_thread == NULL) { return; }
  current_thread->tf = *frame;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(current_thread->tpidr_el0));
}

static void restore_thread(struct thread *thread, struct trap_frame *frame, struct domain *old_domain) {
  if (old_domain != thread->domain) { vmm_install_user(&thread->domain->as); }
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(thread->tpidr_el0));
  *frame = thread->tf;
}

void cell_restore_current(struct trap_frame *frame) {
  if (current_thread == NULL) { return; }
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
    if (!has_blocked) { break; }
    scheduler_waiting_for_interrupt = true;
    __asm__ volatile("msr daifclr, #2\n"
                     "wfi\n"
                     "msr daifset, #2\n"
                     :
                     :
                     : "memory");
    scheduler_waiting_for_interrupt = false;
  }
  kprintf("[kernel] no runnable threads\n");
  poweroff();
  for (;;) {
    __asm__ volatile("wfe");
  }
}

void cell_exit_thread_current(int status, struct trap_frame *frame) {
  if (current_thread == NULL) { return; }
  struct domain *domain = current_thread->domain;
  cleanup_robust_list(current_thread);
  if (current_thread->clear_child_tid != 0) {
    uint32_t zero = 0;
    (void)vmm_copy_to_user(&domain->as, current_thread->clear_child_tid, &zero, sizeof(zero));
    (void)futex_wake(domain, current_thread->clear_child_tid, 1);
  }
  if (runnable_or_blocked_threads_in_domain(domain) <= 1) {
    domain->exit_status = status;
    domain->term_signal = 0;
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
  if (current_thread == NULL) { return; }
  struct domain *domain = current_thread->domain;
  domain->exit_status = status;
  domain->term_signal = 0;
  domain->zombie = true;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (&threads[i] != current_thread && threads[i].domain == domain) {
      threads[i].state = THREAD_UNUSED;
      threads[i].domain = NULL;
      if (domain->refcount > 0) { --domain->refcount; }
    }
  }
  current_thread->state = THREAD_ZOMBIE;
  wake_parent_of(domain);
  cell_schedule(frame);
}

void cell_signal_current(int signal, struct trap_frame *frame) {
  if (current_thread == NULL) { return; }
  struct domain *domain = current_thread->domain;
  domain->exit_status = 128 + signal;
  domain->term_signal = signal;
  domain->zombie = true;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (&threads[i] != current_thread && threads[i].domain == domain) {
      threads[i].state = THREAD_UNUSED;
      threads[i].domain = NULL;
      if (domain->refcount > 0) { --domain->refcount; }
    }
  }
  current_thread->state = THREAD_ZOMBIE;
  wake_parent_of(domain);
  cell_schedule(frame);
}

int cell_fork_current(struct trap_frame *frame) {
  struct domain *parent = current_domain();
  struct domain *child_domain = alloc_domain();
  if (parent == NULL || child_domain == NULL) { return -12; }
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
  copy_cstr(child_domain->name, sizeof(child_domain->name), parent->name);
  return child_domain->id;
}

int cell_clone_thread_current(struct trap_frame *frame, uint64_t flags, uint64_t newsp, uint64_t parent_tid,
                              uint64_t tls, uint64_t child_tid) {
  struct domain *domain = current_domain();
  if (domain == NULL || current_thread == NULL || newsp == 0) { return -22; }
  struct thread *thread = alloc_thread(domain);
  if (thread == NULL) { return -12; }
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
  if (current_thread == NULL) { return 0; }
  current_thread->clear_child_tid = clear_child_tid;
  return current_thread->tid;
}

int cell_set_robust_list_current(uint64_t robust_list) {
  if (current_thread == NULL) { return -22; }
  current_thread->robust_list = robust_list;
  return 0;
}

int cell_futex_wait_current(uint64_t uaddr, uint32_t expected, struct trap_frame *frame) {
  struct domain *domain = current_domain();
  if (domain == NULL || current_thread == NULL || (uaddr & 3u) != 0) { return -EINVAL; }
  if (!cell_ensure_user_range(uaddr, sizeof(uint32_t), VMM_ACCESS_READ)) { return -EFAULT; }
  uint32_t actual = 0;
  if (!vmm_copy_from_user(&domain->as, &actual, uaddr, sizeof(actual))) { return -EFAULT; }
  if (actual != expected) { return -EAGAIN; }
  cell_save_current(frame);
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_FUTEX;
  current_thread->futex_addr = uaddr;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_futex_wake_current(uint64_t uaddr, uint32_t count) {
  struct domain *domain = current_domain();
  if (domain == NULL || (uaddr & 3u) != 0) { return -EINVAL; }
  if (count == 0) { return 0; }
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
    if (domains[i].used && domains[i].parent_id == parent_id && (pid <= 0 || domains[i].id == pid)) { return true; }
  }
  return false;
}

int cell_wait4_options(int pid, uint64_t status_addr, int options, struct trap_frame *frame) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return -ECHILD; }
  struct domain *child = find_waitable_child(domain->id, pid);
  if (child == NULL) {
    if (!has_child(domain->id, pid)) { return -ECHILD; }
    if ((options & WNOHANG) != 0) { return 0; }
    cell_save_current(frame);
    current_thread->state = THREAD_BLOCKED;
    current_thread->wait_reason = WAIT_CHILD;
    current_thread->wait_target = pid;
    cell_schedule(frame);
    return CELL_SWITCHED;
  }

  int status = child->exit_status << 8;
  if (child->term_signal != 0) { status = child->term_signal; }
  if (status_addr != 0 && !vmm_copy_to_user(&domain->as, status_addr, &status, sizeof(status))) { return -14; }
  int child_id = child->id;
  struct thread *child_thread = thread_for_domain(child);
  if (child_thread != NULL) { child_thread->state = THREAD_UNUSED; }
  destroy_domain(child);
  current_thread->wait_target = -1;
  current_thread->wait_reason = WAIT_NONE;
  return child_id;
}

int cell_wait4(int pid, uint64_t status_addr, struct trap_frame *frame) {
  return cell_wait4_options(pid, status_addr, 0, frame);
}

int cell_kill(int pid, int signal) {
  struct domain *domain = find_domain(pid);
  if (domain == NULL || domain->zombie) { return -ESRCH; }
  if (signal == 0) { return 0; }
  if (signal != SIGTERM && signal != SIGINT && signal != SIGKILL && signal != SIGSEGV) { return 0; }
  domain->exit_status = 128 + signal;
  domain->term_signal = signal;
  domain->zombie = true;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].domain == domain) { threads[i].state = THREAD_ZOMBIE; }
  }
  wake_parent_of(domain);
  return 0;
}

bool cell_exec_replace(struct user_address_space *as, struct vma_list *vmas, uint64_t entry, uint64_t sp,
                       struct trap_frame *frame, const char *path, const char *const argv[], uint64_t argc) {
  struct domain *domain = current_domain();
  if (domain == NULL || current_thread == NULL) { return false; }

  struct user_address_space old_as = domain->as;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (&threads[i] != current_thread && threads[i].domain == domain) {
      threads[i].state = THREAD_UNUSED;
      threads[i].domain = NULL;
      if (domain->refcount > 0) { --domain->refcount; }
    }
  }
  domain->as = *as;
  domain->vmas = *vmas;
  set_domain_identity(domain, path, argv, argc);
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
  if (domain == NULL) { return -3; }
  domain->budget.max_ticks = ticks;
  domain->budget.remaining_ticks = ticks;
  if (ticks != 0) { kprintf("[spore] domain %d CPU budget set to %u ticks\n", domain->id, (unsigned)ticks); }
  return 0;
}

void cell_set_boot_epoch(uint64_t epoch_sec) {
  boot_epoch_sec = epoch_sec;
}

void cell_timer_tick(struct trap_frame *frame, bool from_lower_el) {
  ++scheduler_ticks;
  if (scheduler_waiting_for_interrupt) { ++scheduler_idle_ticks; }
  wake_sleep_waiters();
  net_poll();
  wake_poll_waiters();
  struct domain *domain = current_domain();
  if (domain == NULL) { return; }
  if (from_lower_el) { ++domain->cpu_ticks; }
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
  if (from_lower_el) { cell_schedule(frame); }
}

static uint8_t device_random_byte(void) {
  device_rng_state = device_rng_state * 6364136223846793005ull + 1442695040888963407ull;
  return (uint8_t)(device_rng_state >> 32);
}

static int64_t read_stdin_to_user(struct domain *domain, uint64_t buf, uint64_t len);

static const char *thread_state_text(enum thread_state state) {
  switch (state) {
  case THREAD_RUNNABLE:
    return "running";
  case THREAD_BLOCKED:
    return "blocked";
  case THREAD_ZOMBIE:
    return "zombie";
  case THREAD_UNUSED:
    return "unused";
  }
  return "unknown";
}

static const char *wait_reason_text(enum wait_reason reason) {
  switch (reason) {
  case WAIT_NONE:
    return "-";
  case WAIT_CHILD:
    return "child";
  case WAIT_STDIN:
    return "stdin";
  case WAIT_SOCKET:
    return "socket";
  case WAIT_THREAD:
    return "thread";
  case WAIT_FUTEX:
    return "futex";
  case WAIT_POLL:
    return "poll";
  case WAIT_SLEEP:
    return "sleep";
  }
  return "?";
}

static const char *domain_state_text(const struct domain *domain) {
  if (domain == NULL) { return "unknown"; }
  if (domain->zombie) { return "zombie"; }
  bool blocked = false;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].domain != domain || threads[i].state == THREAD_UNUSED) { continue; }
    if (threads[i].state == THREAD_RUNNABLE) { return "running"; }
    if (threads[i].state == THREAD_BLOCKED) { blocked = true; }
  }
  return blocked ? "blocked" : "unknown";
}

static char domain_proc_state_char(const struct domain *domain) {
  const char *state = domain_state_text(domain);
  if (state[0] == 'r') { return 'R'; }
  if (state[0] == 'b') { return 'S'; }
  if (state[0] == 'z') { return 'Z'; }
  return '?';
}

static const char *domain_wait_text(const struct domain *domain) {
  if (domain == NULL || domain->zombie) { return "-"; }
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].domain == domain && threads[i].state == THREAD_BLOCKED) {
      return wait_reason_text(threads[i].wait_reason);
    }
  }
  return "-";
}

static void proc_append_char(char *dst, size_t cap, size_t *len, char c) {
  if (*len + 1 < cap) {
    dst[*len] = c;
    ++*len;
    dst[*len] = '\0';
  }
}

static void proc_append_str(char *dst, size_t cap, size_t *len, const char *s) {
  while (*s != '\0') {
    proc_append_char(dst, cap, len, *s++);
  }
}

static void proc_append_u64(char *dst, size_t cap, size_t *len, uint64_t value) {
  char tmp[32];
  size_t n = 0;
  if (value == 0) {
    proc_append_char(dst, cap, len, '0');
    return;
  }
  while (value != 0 && n < sizeof(tmp)) {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  }
  while (n > 0) {
    proc_append_char(dst, cap, len, tmp[--n]);
  }
}

static void proc_append_u64_pad(char *dst, size_t cap, size_t *len, uint64_t value, size_t width) {
  char tmp[32];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (value % 10));
    value /= 10;
  } while (value != 0 && n < sizeof(tmp));
  while (n < width && n < sizeof(tmp)) {
    tmp[n++] = '0';
  }
  while (n > 0) {
    proc_append_char(dst, cap, len, tmp[--n]);
  }
}

static void proc_append_hex(char *dst, size_t cap, size_t *len, uint64_t value, size_t digits) {
  static const char hex[] = "0123456789abcdef";
  proc_append_str(dst, cap, len, "0x");
  for (size_t i = 0; i < digits; ++i) {
    size_t shift = (digits - i - 1) * 4;
    proc_append_char(dst, cap, len, hex[(value >> shift) & 0xf]);
  }
}

static size_t procinfo_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "pid ppid state wait rss_pages cpu_ticks age_ticks budget_remaining budget_max name exec_path cwd "
                  "cmdline\n");
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    const struct domain *domain = &domains[i];
    if (!domain->used) { continue; }
    proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain_state_text(domain));
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain_wait_text(domain));
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain_resident_pages(domain));
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->cpu_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, scheduler_ticks - domain->start_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->budget.remaining_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_u64(dst, cap, &len, domain->budget.max_ticks);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->name);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? "-" : domain->exec_path);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->cwd);
    proc_append_char(dst, cap, &len, ' ');
    proc_append_str(dst, cap, &len, domain->cmdline[0] == '\0' ? domain->name : domain->cmdline);
    proc_append_char(dst, cap, &len, '\n');
  }
  return len;
}

static const char *cpu_implementer_name(uint64_t implementer) {
  switch (implementer) {
  case 0x41:
    return "ARM";
  case 0x42:
    return "Broadcom";
  case 0x43:
    return "Cavium";
  case 0x46:
    return "Fujitsu";
  case 0x48:
    return "HiSilicon";
  case 0x4e:
    return "NVIDIA";
  case 0x50:
    return "AppliedMicro";
  case 0x51:
    return "Qualcomm";
  case 0x53:
    return "Samsung";
  case 0x56:
    return "Marvell";
  case 0x61:
    return "Apple";
  case 0x69:
    return "Intel";
  default:
    return "Unknown";
  }
}

static const char *arm_cpu_part_name(uint64_t part) {
  switch (part) {
  case 0xd03:
    return "ARM Cortex-A53";
  case 0xd04:
    return "ARM Cortex-A35";
  case 0xd05:
    return "ARM Cortex-A55";
  case 0xd07:
    return "ARM Cortex-A57";
  case 0xd08:
    return "ARM Cortex-A72";
  case 0xd09:
    return "ARM Cortex-A73";
  case 0xd0a:
    return "ARM Cortex-A75";
  case 0xd0b:
    return "ARM Cortex-A76";
  case 0xd0c:
    return "ARM Neoverse-N1";
  case 0xd40:
    return "ARM Neoverse-V1";
  case 0xd41:
    return "ARM Cortex-A78";
  case 0xd44:
    return "ARM Cortex-X1";
  case 0xd46:
    return "ARM Cortex-A510";
  case 0xd47:
    return "ARM Cortex-A710";
  case 0xd48:
    return "ARM Cortex-X2";
  case 0xd49:
    return "ARM Neoverse-N2";
  case 0xd4a:
    return "ARM Neoverse-E1";
  case 0xd4b:
    return "ARM Cortex-A78C";
  case 0xd4d:
    return "ARM Cortex-A715";
  case 0xd4e:
    return "ARM Cortex-X3";
  case 0xd4f:
    return "ARM Neoverse-V2";
  case 0xd80:
    return "ARM Cortex-A520";
  case 0xd81:
    return "ARM Cortex-A720";
  case 0xd82:
    return "ARM Cortex-X4";
  default:
    return "ARM AArch64 CPU";
  }
}

static const char *cpu_model_name(uint64_t implementer, uint64_t part) {
  if (implementer == 0x41) { return arm_cpu_part_name(part); }
  if (implementer == 0x61) { return "Apple AArch64 CPU"; }
  return "AArch64 CPU";
}

static void proc_append_cpu_features(char *dst, size_t cap, size_t *len, uint64_t pfr0, uint64_t isar0) {
  bool first = true;
#define APPEND_FEATURE(name)                                                                                           \
  do {                                                                                                                 \
    if (!first) { proc_append_char(dst, cap, len, ' '); }                                                              \
    proc_append_str(dst, cap, len, name);                                                                              \
    first = false;                                                                                                     \
  } while (0)
  uint64_t fp = (pfr0 >> 16) & 0xf;
  uint64_t asimd = (pfr0 >> 20) & 0xf;
  uint64_t aes = (isar0 >> 4) & 0xf;
  uint64_t sha1 = (isar0 >> 8) & 0xf;
  uint64_t sha2 = (isar0 >> 12) & 0xf;
  uint64_t crc32 = (isar0 >> 16) & 0xf;
  uint64_t atomic = (isar0 >> 20) & 0xf;
  uint64_t rndr = (isar0 >> 60) & 0xf;
  if (fp != 0xf) { APPEND_FEATURE("fp"); }
  if (asimd != 0xf) { APPEND_FEATURE("asimd"); }
  if (aes >= 1) { APPEND_FEATURE("aes"); }
  if (aes >= 2) { APPEND_FEATURE("pmull"); }
  if (sha1 >= 1) { APPEND_FEATURE("sha1"); }
  if (sha2 >= 1) { APPEND_FEATURE("sha2"); }
  if (crc32 >= 1) { APPEND_FEATURE("crc32"); }
  if (atomic >= 2) { APPEND_FEATURE("atomics"); }
  if (rndr >= 1) { APPEND_FEATURE("rng"); }
  if (first) { proc_append_str(dst, cap, len, "none"); }
#undef APPEND_FEATURE
}

static size_t meminfo_text(char *dst, size_t cap) {
  size_t len = 0;
  uint64_t total_pages = pmm_total_pages();
  uint64_t free_pages = pmm_free_pages();
  uint64_t used_pages = total_pages > free_pages ? total_pages - free_pages : 0;
  uint64_t buffer_pages = ext2_cache_used_pages();
  uint64_t cached_pages = vfs_exec_cache_pages();
  uint64_t shmem_pages = ramfs_backing_used_pages();
  uint64_t reclaimable_pages = buffer_pages + cached_pages;
  uint64_t inactive_pages = reclaimable_pages + shmem_pages;
  uint64_t active_pages = used_pages > inactive_pages ? used_pages - inactive_pages : 0;
  uint64_t total_kib = total_pages * 4;
  uint64_t free_kib = free_pages * 4;
  uint64_t used_kib = used_pages * 4;
  uint64_t buffer_kib = buffer_pages * 4;
  uint64_t cached_kib = cached_pages * 4;
  uint64_t shmem_kib = shmem_pages * 4;
  uint64_t reclaimable_kib = reclaimable_pages * 4;
  uint64_t active_kib = active_pages * 4;
  uint64_t inactive_kib = inactive_pages * 4;
  uint64_t available_kib = free_kib + reclaimable_kib;
  proc_append_str(dst, cap, &len, "MemTotal: ");
  proc_append_u64(dst, cap, &len, total_kib);
  proc_append_str(dst, cap, &len, " kB\nMemFree: ");
  proc_append_u64(dst, cap, &len, free_kib);
  proc_append_str(dst, cap, &len, " kB\nMemAvailable: ");
  proc_append_u64(dst, cap, &len, available_kib);
  proc_append_str(dst, cap, &len, " kB\nBuffers: ");
  proc_append_u64(dst, cap, &len, buffer_kib);
  proc_append_str(dst, cap, &len, " kB\nCached: ");
  proc_append_u64(dst, cap, &len, cached_kib);
  proc_append_str(dst, cap, &len, " kB\nSwapCached: 0 kB\nActive: ");
  proc_append_u64(dst, cap, &len, active_kib);
  proc_append_str(dst, cap, &len, " kB\nInactive: ");
  proc_append_u64(dst, cap, &len, inactive_kib);
  proc_append_str(dst, cap, &len, " kB\nShmem: ");
  proc_append_u64(dst, cap, &len, shmem_kib);
  proc_append_str(dst, cap, &len, " kB\nSReclaimable: ");
  proc_append_u64(dst, cap, &len, reclaimable_kib);
  proc_append_str(dst, cap, &len, " kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n");
  proc_append_str(dst, cap, &len, "MemTotalPages: ");
  proc_append_u64(dst, cap, &len, total_pages);
  proc_append_str(dst, cap, &len, "\nMemFreePages: ");
  proc_append_u64(dst, cap, &len, free_pages);
  proc_append_str(dst, cap, &len, "\nMemUsedPages: ");
  proc_append_u64(dst, cap, &len, used_pages);
  proc_append_str(dst, cap, &len, "\nMemTotalKiB: ");
  proc_append_u64(dst, cap, &len, total_kib);
  proc_append_str(dst, cap, &len, "\nMemUsedKiB: ");
  proc_append_u64(dst, cap, &len, used_kib);
  proc_append_str(dst, cap, &len, "\nMemFreeKiB: ");
  proc_append_u64(dst, cap, &len, free_kib);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t cpuinfo_text(char *dst, size_t cap) {
  size_t len = 0;
  uint64_t midr = 0;
  uint64_t pfr0 = 0;
  uint64_t isar0 = 0;
  uint64_t cntfrq = 0;
  __asm__ volatile("mrs %0, MIDR_EL1" : "=r"(midr));
  __asm__ volatile("mrs %0, ID_AA64PFR0_EL1" : "=r"(pfr0));
  __asm__ volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(isar0));
  __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(cntfrq));

  uint64_t implementer = (midr >> 24) & 0xff;
  uint64_t variant = (midr >> 20) & 0xf;
  uint64_t architecture = (midr >> 16) & 0xf;
  uint64_t linux_architecture = architecture == 0xf ? 8 : architecture;
  uint64_t part = (midr >> 4) & 0xfff;
  uint64_t revision = midr & 0xf;
  const char *model = cpu_model_name(implementer, part);

  proc_append_str(dst, cap, &len, "processor\t: 0\nvendor_id\t: ");
  proc_append_str(dst, cap, &len, cpu_implementer_name(implementer));
  proc_append_str(dst, cap, &len, "\nmodel name\t: ");
  proc_append_str(dst, cap, &len, model);
  proc_append_str(dst, cap, &len, "\nBogoMIPS\t: ");
  proc_append_u64(dst, cap, &len, cntfrq / 500000);
  proc_append_str(dst, cap, &len, ".00\nFeatures\t: ");
  proc_append_cpu_features(dst, cap, &len, pfr0, isar0);
  proc_append_str(dst, cap, &len, "\nCPU implementer\t: ");
  proc_append_hex(dst, cap, &len, implementer, 2);
  proc_append_str(dst, cap, &len, "\nCPU architecture: ");
  proc_append_u64(dst, cap, &len, linux_architecture);
  proc_append_str(dst, cap, &len, "\nCPU variant\t: ");
  proc_append_hex(dst, cap, &len, variant, 1);
  proc_append_str(dst, cap, &len, "\nCPU part\t: ");
  proc_append_hex(dst, cap, &len, part, 3);
  proc_append_str(dst, cap, &len, "\nCPU revision\t: ");
  proc_append_u64(dst, cap, &len, revision);
  proc_append_str(dst, cap, &len, "\nHardware\t: ");
  proc_append_str(dst, cap, &len, model);
  proc_append_str(dst, cap, &len, "\ncpu MHz\t\t: ");
  proc_append_u64(dst, cap, &len, cntfrq / 1000000);
  proc_append_char(dst, cap, &len, '.');
  proc_append_u64_pad(dst, cap, &len, (cntfrq % 1000000) / 1000, 3);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t uptime_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_u64(dst, cap, &len, scheduler_ticks / 100);
  proc_append_char(dst, cap, &len, '.');
  if ((scheduler_ticks % 100) < 10) { proc_append_char(dst, cap, &len, '0'); }
  proc_append_u64(dst, cap, &len, scheduler_ticks % 100);
  proc_append_str(dst, cap, &len, " 0.00\n");
  return len;
}

static size_t loadavg_text(char *dst, size_t cap) {
  size_t len = 0;
  uint64_t runnable = 0;
  uint64_t total = 0;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].domain == NULL) { continue; }
    ++total;
    if (threads[i].state == THREAD_RUNNABLE) { ++runnable; }
  }
  proc_append_str(dst, cap, &len, "0.00 0.00 0.00 ");
  proc_append_u64(dst, cap, &len, runnable);
  proc_append_char(dst, cap, &len, '/');
  proc_append_u64(dst, cap, &len, total);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, (uint32_t)(next_domain_id - 1));
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t mounts_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "ext2-root / ext2 rw 0 0\n"
                  "tmpfs /tmp tmpfs rw 0 0\n"
                  "bootfs /dev/fs/boot fat16 ro 0 0\n"
                  "ramfs /dev/fs/ram0 ramfs ro 0 0\n"
                  "proc /proc proc ro 0 0\n"
                  "dev /dev devfs rw 0 0\n");
  return len;
}

static size_t net_dev_text(char *dst, size_t cap) {
  size_t len = 0;
  struct virtio_net_stats stats = virtio_net_stats();
  proc_append_str(dst, cap, &len,
                  "Inter-|   Receive                                                |  Transmit\n"
                  " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo "
                  "colls carrier compressed\n"
                  "  eth0: ");
  proc_append_u64(dst, cap, &len, stats.rx_bytes);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, stats.rx_packets);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0 ");
  proc_append_u64(dst, cap, &len, stats.tx_bytes);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, stats.tx_packets);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0\n");
  return len;
}

static size_t filesystems_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "nodev\tproc\n"
                  "nodev\tdevfs\n"
                  "nodev\ttmpfs\n"
                  "nodev\tramfs\n"
                  "\text2\n");
  return len;
}

static size_t partitions_text(char *dst, size_t cap) {
  size_t len = 0;
  struct vfs_fs_info info;
  uint64_t root_blocks = 0;
  if (vfs_fs_info(&info) && info.block_size != 0) { root_blocks = (info.block_count * info.block_size) / 1024; }
  proc_append_str(dst, cap, &len,
                  "major minor  #blocks  name\n"
                  " 254     0   ");
  proc_append_u64(dst, cap, &len, root_blocks);
  proc_append_str(dst, cap, &len,
                  "  vda\n"
                  " 254     1    16384  vdb\n");
  return len;
}

static size_t devices_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len,
                  "Character devices:\n"
                  "  1 mem\n"
                  "  5 tty\n"
                  " 10 misc\n"
                  "\nBlock devices:\n"
                  "254 virtblk\n");
  return len;
}

static size_t fs_root_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "ext2 root filesystem mounted at /\n");
  return len;
}

static size_t fs_boot_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "ESP boot filesystem image used by UEFI\n");
  return len;
}

static size_t fs_ram0_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "synthetic boot ramfs for devfs/procfs and fallback modules\n");
  return len;
}

static size_t fs_tmp_text(char *dst, size_t cap) {
  size_t len = 0;
  proc_append_str(dst, cap, &len, "tmpfs mounted at /tmp\n");
  return len;
}

static uint64_t total_domain_cpu_ticks(void) {
  uint64_t ticks = 0;
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    if (domains[i].used) { ticks += domains[i].cpu_ticks; }
  }
  return ticks;
}

static size_t stat_text(char *dst, size_t cap) {
  size_t len = 0;
  uint64_t idle_ticks = scheduler_idle_ticks > scheduler_ticks ? scheduler_ticks : scheduler_idle_ticks;
  uint64_t busy_ticks = scheduler_ticks - idle_ticks;
  uint64_t running = 0;
  uint64_t blocked = 0;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].state == THREAD_RUNNABLE) {
      ++running;
    } else if (threads[i].state == THREAD_BLOCKED) {
      ++blocked;
    }
  }
  proc_append_str(dst, cap, &len, "cpu  ");
  proc_append_u64(dst, cap, &len, busy_ticks);
  proc_append_str(dst, cap, &len, " 0 0 ");
  proc_append_u64(dst, cap, &len, idle_ticks);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0\ncpu0 ");
  proc_append_u64(dst, cap, &len, busy_ticks);
  proc_append_str(dst, cap, &len, " 0 0 ");
  proc_append_u64(dst, cap, &len, idle_ticks);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0\nctxt ");
  proc_append_u64(dst, cap, &len, scheduler_ticks);
  proc_append_str(dst, cap, &len, "\nbtime ");
  proc_append_u64(dst, cap, &len, boot_epoch_sec);
  proc_append_str(dst, cap, &len, "\nprocesses ");
  proc_append_u64(dst, cap, &len, (uint32_t)(next_domain_id - 1));
  proc_append_str(dst, cap, &len, "\nprocs_running ");
  proc_append_u64(dst, cap, &len, running == 0 ? 1 : running);
  proc_append_str(dst, cap, &len, "\nprocs_blocked ");
  proc_append_u64(dst, cap, &len, blocked);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static const struct domain *domain_for_pid(int pid) {
  return find_domain(pid);
}

static size_t proc_pid_status_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, "Name:\t");
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_str(dst, cap, &len, "\nState:\t");
  proc_append_str(dst, cap, &len, domain_state_text(domain));
  proc_append_str(dst, cap, &len, "\nPid:\t");
  proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
  proc_append_str(dst, cap, &len, "\nPPid:\t");
  proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
  proc_append_str(dst, cap, &len, "\nUid:\t");
  proc_append_u64(dst, cap, &len, domain->uid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->euid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->uid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->euid);
  proc_append_str(dst, cap, &len, "\nGid:\t");
  proc_append_u64(dst, cap, &len, domain->gid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->egid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->gid);
  proc_append_char(dst, cap, &len, '\t');
  proc_append_u64(dst, cap, &len, domain->egid);
  proc_append_str(dst, cap, &len, "\nVmRSSPages:\t");
  proc_append_u64(dst, cap, &len, domain_resident_pages(domain));
  proc_append_str(dst, cap, &len, "\nCpuTicks:\t");
  proc_append_u64(dst, cap, &len, domain->cpu_ticks);
  proc_append_str(dst, cap, &len, "\nCwd:\t");
  proc_append_str(dst, cap, &len, domain->cwd);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_stat_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  uint64_t rss = domain_resident_pages(domain);
  uint64_t start_time = domain->start_ticks;
  uint64_t utime = domain->cpu_ticks;
  proc_append_u64(dst, cap, &len, (uint32_t)domain->id);
  proc_append_str(dst, cap, &len, " (");
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_str(dst, cap, &len, ") ");
  proc_append_char(dst, cap, &len, domain_proc_state_char(domain));
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, (uint32_t)domain->parent_id);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0 0 0 0 0 ");
  proc_append_u64(dst, cap, &len, utime);
  proc_append_str(dst, cap, &len, " 0 0 0 20 0 1 0 ");
  proc_append_u64(dst, cap, &len, start_time);
  proc_append_str(dst, cap, &len, " ");
  proc_append_u64(dst, cap, &len, rss * PAGE_SIZE);
  proc_append_str(dst, cap, &len, " ");
  proc_append_u64(dst, cap, &len, rss);
  proc_append_str(dst, cap, &len, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
  return len;
}

static size_t proc_pid_cmdline_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? domain->name : domain->exec_path);
  proc_append_char(dst, cap, &len, '\0');
  return len;
}

static size_t proc_pid_statm_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  uint64_t rss = domain_resident_pages(domain);
  uint64_t virt = rss == 0 ? 1 : rss;
  proc_append_u64(dst, cap, &len, virt);
  proc_append_char(dst, cap, &len, ' ');
  proc_append_u64(dst, cap, &len, rss);
  proc_append_str(dst, cap, &len, " 0 0 0 ");
  proc_append_u64(dst, cap, &len, rss);
  proc_append_str(dst, cap, &len, " 0\n");
  return len;
}

static size_t proc_pid_comm_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->name);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_cwd_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->cwd);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static size_t proc_pid_exe_text(char *dst, size_t cap, int pid) {
  size_t len = 0;
  const struct domain *domain = domain_for_pid(pid);
  if (domain == NULL) { return 0; }
  proc_append_str(dst, cap, &len, domain->exec_path[0] == '\0' ? "-" : domain->exec_path);
  proc_append_char(dst, cap, &len, '\n');
  return len;
}

static int64_t read_generated_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len,
                                     size_t (*fill)(char *, size_t)) {
  char text[2048] = {0};
  size_t text_len = fill(text, sizeof(text));
  if (file->offset >= text_len) { return 0; }
  size_t chunk = text_len - (size_t)file->offset;
  if (chunk > len) { chunk = (size_t)len; }
  if (!vmm_copy_to_user(&domain->as, buf, text + file->offset, chunk)) { return -14; }
  file->offset += chunk;
  return (int64_t)chunk;
}

static int64_t read_generated_pid_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len,
                                         size_t (*fill)(char *, size_t, int)) {
  char text[2048] = {0};
  size_t text_len = fill(text, sizeof(text), file->node.proc_pid);
  if (file->offset >= text_len) { return 0; }
  size_t chunk = text_len - (size_t)file->offset;
  if (chunk > len) { chunk = (size_t)len; }
  if (!vmm_copy_to_user(&domain->as, buf, text + file->offset, chunk)) { return -14; }
  file->offset += chunk;
  return (int64_t)chunk;
}

static int64_t write_console_from_user(struct domain *domain, uint64_t buf, uint64_t len) {
  for (uint64_t i = 0; i < len; ++i) {
    char c;
    if (!vmm_copy_from_user(&domain->as, &c, buf + i, 1)) { return -14; }
    pl011_putc(c);
    if (c == '\r' || c == '\n') {
      tty_output_line_len = 0;
      tty_output_line[0] = '\0';
    } else if ((uint8_t)c >= 0x20 || c == '\t' || c == '\033') {
      if (tty_output_line_len + 1 < sizeof(tty_output_line)) {
        tty_output_line[tty_output_line_len++] = c;
        tty_output_line[tty_output_line_len] = '\0';
      }
    }
  }
  return (int64_t)len;
}

static int64_t write_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len) {
  switch (file->node.device) {
  case RAMFS_DEV_NULL:
  case RAMFS_DEV_ZERO:
  case RAMFS_DEV_RANDOM:
  case RAMFS_DEV_URANDOM:
    return (int64_t)len;
  case RAMFS_DEV_FULL:
    return -28;
  case RAMFS_DEV_CONSOLE:
  case RAMFS_DEV_TTY:
    return write_console_from_user(domain, buf, len);
  case RAMFS_DEV_PROCINFO:
  case RAMFS_DEV_MEMINFO:
  case RAMFS_DEV_CPUINFO:
  case RAMFS_DEV_UPTIME:
  case RAMFS_DEV_LOADAVG:
  case RAMFS_DEV_MOUNTS:
  case RAMFS_DEV_STAT:
  case RAMFS_DEV_NET_DEV:
  case RAMFS_DEV_FILESYSTEMS:
  case RAMFS_DEV_PARTITIONS:
  case RAMFS_DEV_DEVICES:
  case RAMFS_DEV_PROC_PID_STAT:
  case RAMFS_DEV_PROC_PID_STATUS:
  case RAMFS_DEV_PROC_PID_CMDLINE:
  case RAMFS_DEV_PROC_PID_STATM:
  case RAMFS_DEV_PROC_PID_COMM:
  case RAMFS_DEV_PROC_PID_MOUNTS:
  case RAMFS_DEV_PROC_PID_CWD:
  case RAMFS_DEV_PROC_PID_EXE:
  case RAMFS_DEV_FS_ROOT:
  case RAMFS_DEV_FS_BOOT:
  case RAMFS_DEV_FS_RAM0:
  case RAMFS_DEV_FS_TMP:
  case RAMFS_DEV_BLK_BOOT:
    return -22;
  case RAMFS_DEV_BLK_ROOT: {
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
      if (!vmm_copy_from_user(&domain->as, tmp, buf + done, (size_t)chunk)) { return -14; }
      if (!virtio_blk_write(file->offset, tmp, (uint32_t)chunk)) { return -5; }
      file->offset += chunk;
      done += chunk;
    }
    return (int64_t)done;
  }
  case RAMFS_DEV_NONE:
    break;
  }
  return -22;
}

static int64_t read_device(struct open_file *file, struct domain *domain, uint64_t buf, uint64_t len,
                           struct trap_frame *frame) {
  if (len == 0) { return 0; }
  switch (file->node.device) {
  case RAMFS_DEV_NULL:
    return 0;
  case RAMFS_DEV_ZERO:
  case RAMFS_DEV_FULL:
  case RAMFS_DEV_RANDOM:
  case RAMFS_DEV_URANDOM:
    for (uint64_t i = 0; i < len; ++i) {
      uint8_t byte =
        file->node.device == RAMFS_DEV_RANDOM || file->node.device == RAMFS_DEV_URANDOM ? device_random_byte() : 0;
      if (!vmm_copy_to_user(&domain->as, buf + i, &byte, 1)) { return -14; }
    }
    return (int64_t)len;
  case RAMFS_DEV_CONSOLE:
  case RAMFS_DEV_TTY: {
    int64_t n = read_stdin_to_user(domain, buf, len);
    if (n != 0) { return n; }
    if (frame == NULL) { return 0; }
    cell_save_current(frame);
    current_thread->state = THREAD_BLOCKED;
    current_thread->wait_reason = WAIT_STDIN;
    current_thread->stdin_buf = buf;
    current_thread->stdin_len = len;
    cell_schedule(frame);
    return CELL_SWITCHED;
  }
  case RAMFS_DEV_PROCINFO:
    return read_generated_device(file, domain, buf, len, procinfo_text);
  case RAMFS_DEV_MEMINFO:
    return read_generated_device(file, domain, buf, len, meminfo_text);
  case RAMFS_DEV_CPUINFO:
    return read_generated_device(file, domain, buf, len, cpuinfo_text);
  case RAMFS_DEV_UPTIME:
    return read_generated_device(file, domain, buf, len, uptime_text);
  case RAMFS_DEV_LOADAVG:
    return read_generated_device(file, domain, buf, len, loadavg_text);
  case RAMFS_DEV_MOUNTS:
    return read_generated_device(file, domain, buf, len, mounts_text);
  case RAMFS_DEV_STAT:
    return read_generated_device(file, domain, buf, len, stat_text);
  case RAMFS_DEV_NET_DEV:
    return read_generated_device(file, domain, buf, len, net_dev_text);
  case RAMFS_DEV_FILESYSTEMS:
    return read_generated_device(file, domain, buf, len, filesystems_text);
  case RAMFS_DEV_PARTITIONS:
    return read_generated_device(file, domain, buf, len, partitions_text);
  case RAMFS_DEV_DEVICES:
    return read_generated_device(file, domain, buf, len, devices_text);
  case RAMFS_DEV_PROC_PID_STAT:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_stat_text);
  case RAMFS_DEV_PROC_PID_STATUS:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_status_text);
  case RAMFS_DEV_PROC_PID_CMDLINE:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_cmdline_text);
  case RAMFS_DEV_PROC_PID_STATM:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_statm_text);
  case RAMFS_DEV_PROC_PID_COMM:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_comm_text);
  case RAMFS_DEV_PROC_PID_MOUNTS:
    return read_generated_device(file, domain, buf, len, mounts_text);
  case RAMFS_DEV_PROC_PID_CWD:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_cwd_text);
  case RAMFS_DEV_PROC_PID_EXE:
    return read_generated_pid_device(file, domain, buf, len, proc_pid_exe_text);
  case RAMFS_DEV_FS_ROOT:
    return read_generated_device(file, domain, buf, len, fs_root_text);
  case RAMFS_DEV_FS_BOOT:
    return read_generated_device(file, domain, buf, len, fs_boot_text);
  case RAMFS_DEV_FS_RAM0:
    return read_generated_device(file, domain, buf, len, fs_ram0_text);
  case RAMFS_DEV_FS_TMP:
    return read_generated_device(file, domain, buf, len, fs_tmp_text);
  case RAMFS_DEV_BLK_ROOT: {
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
      if (!virtio_blk_read(file->offset, tmp, (uint32_t)chunk)) { return done == 0 ? -5 : (int64_t)done; }
      if (!vmm_copy_to_user(&domain->as, buf + done, tmp, (size_t)chunk)) { return -14; }
      file->offset += chunk;
      done += chunk;
    }
    return (int64_t)done;
  }
  case RAMFS_DEV_BLK_BOOT: {
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
      if (!virtio_blk_read_boot(file->offset, tmp, (uint32_t)chunk)) { return done == 0 ? -5 : (int64_t)done; }
      if (!vmm_copy_to_user(&domain->as, buf + done, tmp, (size_t)chunk)) { return -14; }
      file->offset += chunk;
      done += chunk;
    }
    return (int64_t)done;
  }
  case RAMFS_DEV_NONE:
    break;
  }
  return -22;
}

bool cell_handle_cow_fault(uint64_t far) {
  struct domain *domain = current_domain();
  return domain != NULL && vmm_handle_cow_fault(&domain->as, far);
}

int64_t cell_fd_write(int fd, uint64_t buf, uint64_t len) {
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_STDOUT) {
    if (file->type != OPEN_RAMFS ||
        ((file->flags & CELL_O_ACCMODE) != CELL_O_WRONLY && (file->flags & CELL_O_ACCMODE) != CELL_O_RDWR)) {
      return -22;
    }
    if (file->node.device != RAMFS_DEV_NONE) { return write_device(file, domain, buf, len); }
    if ((file->flags & CELL_O_APPEND) != 0) {
      struct vfs_node fresh;
      if (vfs_refresh(&file->node, &fresh)) { file->offset = fresh.size; }
    }
    uint8_t tmp[128];
    uint64_t done = 0;
    while (done < len) {
      uint64_t chunk = len - done;
      if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
      if (!vmm_copy_from_user(&domain->as, tmp, buf + done, (size_t)chunk)) { return -14; }
      int64_t wrote = vfs_write(&file->node, file->offset, tmp, chunk);
      if (wrote < 0) { return -28; }
      file->offset += (uint64_t)wrote;
      done += (uint64_t)wrote;
      (void)vfs_refresh(&file->node, &file->node);
    }
    return (int64_t)done;
  }
  return write_console_from_user(domain, buf, len);
}

static bool tty_canonical(void) {
  return (tty_lflag & TTY_ICANON) != 0;
}

static bool tty_echo(void) {
  return (tty_lflag & TTY_ECHO) != 0;
}

uint32_t cell_tty_lflag(void) {
  return tty_lflag;
}

void cell_tty_set_lflag(uint32_t lflag) {
  tty_lflag = lflag;
  if (!tty_canonical()) {
    tty_line_len = 0;
    tty_line_cursor = 0;
    tty_ready_head = 0;
    tty_ready_len = 0;
    tty_prompt_active = false;
  }
}

static void tty_begin_canonical_read(void) {
  if (tty_prompt_active) { return; }
  tty_prompt_len = tty_output_line_len;
  if (tty_prompt_len >= sizeof(tty_prompt)) { tty_prompt_len = sizeof(tty_prompt) - 1; }
  for (size_t i = 0; i < tty_prompt_len; ++i) {
    tty_prompt[i] = tty_output_line[i];
  }
  tty_prompt[tty_prompt_len] = '\0';
  tty_line_cursor = tty_line_len;
  tty_prompt_active = true;
}

static void tty_echo_char(char c) {
  if (!tty_echo()) { return; }
  if (c == '\n') {
    pl011_putc('\n');
    tty_output_line_len = 0;
    tty_output_line[0] = '\0';
  } else if (c == '\b' || c == 0x7f) {
    pl011_putc('\b');
    pl011_putc(' ');
    pl011_putc('\b');
  } else if ((uint8_t)c >= 0x20 || c == '\t') {
    pl011_putc(c);
  }
}

static void tty_echo_str(const char *s) {
  if (!tty_echo()) { return; }
  while (*s != '\0') {
    pl011_putc(*s++);
  }
}

static void tty_redraw_line(void) {
  tty_echo_str("\r\033[K");
  for (size_t i = 0; i < tty_prompt_len; ++i) {
    pl011_putc(tty_prompt[i]);
  }
  for (size_t i = 0; i < tty_line_len; ++i) {
    pl011_putc(tty_line[i]);
  }
  if (tty_line_cursor < tty_line_len) {
    tty_echo_str("\033[");
    size_t move = tty_line_len - tty_line_cursor;
    char rev[20];
    size_t n = 0;
    do {
      rev[n++] = (char)('0' + (move % 10u));
      move /= 10u;
    } while (move != 0 && n < sizeof(rev));
    while (n > 0) {
      pl011_putc(rev[n - 1]);
      --n;
    }
    pl011_putc('D');
  }
}

static void tty_ready_push(char c) {
  if (tty_ready_len >= sizeof(tty_ready)) { return; }
  size_t index = (tty_ready_head + tty_ready_len) % sizeof(tty_ready);
  tty_ready[index] = c;
  ++tty_ready_len;
}

static bool tty_ready_pop(char *out) {
  if (tty_ready_len == 0) { return false; }
  *out = tty_ready[tty_ready_head];
  tty_ready_head = (tty_ready_head + 1u) % sizeof(tty_ready);
  --tty_ready_len;
  return true;
}

static void tty_line_commit(void) {
  for (size_t i = 0; i < tty_line_len; ++i) {
    tty_ready_push(tty_line[i]);
  }
  tty_ready_push('\n');
  tty_line_len = 0;
  tty_line_cursor = 0;
  tty_prompt_active = false;
}

static bool tty_try_escape(void) {
  char bracket;
  if (!pl011_getc(&bracket)) { return true; }
  if (bracket != '[') { return true; }

  char final = '\0';
  while (pl011_getc(&final)) {
    if (final >= '@' && final <= '~') { break; }
  }

  if (final == 'C') {
    if (tty_line_cursor < tty_line_len) {
      ++tty_line_cursor;
      tty_echo_str("\033[C");
    }
  } else if (final == 'D') {
    if (tty_line_cursor > 0) {
      --tty_line_cursor;
      tty_echo_str("\033[D");
    }
  }
  return true;
}

static void tty_process_input(void) {
  char c;
  while (tty_ready_len == 0 && pl011_getc(&c)) {
    if (!tty_canonical()) {
      tty_ready_push(c);
      return;
    }
    if (c == 0x1b) {
      (void)tty_try_escape();
      continue;
    }
    if (c == '\r') { c = '\n'; }
    if (c == '\n') {
      tty_echo_char('\n');
      tty_line_commit();
      return;
    }
    if (c == 3) {
      if (tty_echo()) {
        pl011_putc('^');
        pl011_putc('C');
        pl011_putc('\n');
      }
      tty_line_len = 0;
      tty_line_cursor = 0;
      tty_prompt_active = false;
      tty_ready_push('\n');
      return;
    }
    if (c == '\b' || c == 0x7f) {
      if (tty_line_cursor > 0) {
        for (size_t i = tty_line_cursor - 1; i + 1 < tty_line_len; ++i) {
          tty_line[i] = tty_line[i + 1];
        }
        --tty_line_len;
        --tty_line_cursor;
        tty_redraw_line();
      }
      continue;
    }
    if ((uint8_t)c < 0x20 && c != '\t') { continue; }
    if (tty_line_len + 1 < sizeof(tty_line)) {
      for (size_t i = tty_line_len; i > tty_line_cursor; --i) {
        tty_line[i] = tty_line[i - 1];
      }
      tty_line[tty_line_cursor++] = c;
      ++tty_line_len;
      if (tty_line_cursor == tty_line_len) {
        tty_echo_char(c);
      } else {
        tty_redraw_line();
      }
    }
  }
}

static int64_t read_stdin_to_user(struct domain *domain, uint64_t buf, uint64_t len) {
  if (tty_canonical()) { tty_begin_canonical_read(); }
  uint64_t n = 0;
  while (n < len) {
    char c;
    if (!tty_ready_pop(&c)) {
      tty_process_input();
      if (!tty_ready_pop(&c)) { break; }
    }
    if (!vmm_copy_to_user(&domain->as, buf + n, &c, 1)) { return -14; }
    ++n;
    if (tty_canonical() && c == '\n') { break; }
  }
  return (int64_t)n;
}

static bool tty_stdin_readable(void) {
  if (tty_ready_len != 0) { return true; }
  tty_process_input();
  return tty_ready_len != 0;
}

int64_t cell_fd_read(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame) {
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_STDIN) {
    if (len == 0) { return 0; }
    int64_t n = read_stdin_to_user(domain, buf, len);
    if (n != 0) { return n; }
    if (frame == NULL) { return 0; }
    cell_save_current(frame);
    current_thread->state = THREAD_BLOCKED;
    current_thread->wait_reason = WAIT_STDIN;
    current_thread->stdin_buf = buf;
    current_thread->stdin_len = len;
    cell_schedule(frame);
    return CELL_SWITCHED;
  }
  if (file->type != OPEN_RAMFS || file->node.is_dir) { return -22; }
  if (file->node.device != RAMFS_DEV_NONE) { return read_device(file, domain, buf, len, frame); }
  uint8_t tmp[128];
  uint64_t done = 0;
  while (done < len) {
    uint64_t chunk = len - done;
    if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
    uint64_t got = vfs_read(&file->node, file->offset, tmp, chunk);
    if (got == 0) { break; }
    if (!vmm_copy_to_user(&domain->as, buf + done, tmp, (size_t)got)) { return -14; }
    file->offset += got;
    done += got;
  }
  return (int64_t)done;
}

static int fd_poll_events_for_domain(struct domain *domain, int fd, int events) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return CELL_POLLNVAL; }

  struct open_file *file = domain->fds[fd];
  int revents = 0;
  if ((events & CELL_POLLIN) != 0) {
    if (file->type == OPEN_STDIN ||
        (file->type == OPEN_RAMFS && (file->node.device == RAMFS_DEV_TTY || file->node.device == RAMFS_DEV_CONSOLE))) {
      if (tty_stdin_readable()) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_SOCKET) {
      net_poll();
      if (file->udp_rx_len != 0) { revents |= CELL_POLLIN; }
    } else if (file->type == OPEN_RAMFS) {
      revents |= CELL_POLLIN;
    }
  }

  if ((events & CELL_POLLOUT) != 0) {
    if (file->type == OPEN_STDOUT || file->type == OPEN_STDIN || file->type == OPEN_RAMFS ||
        file->type == OPEN_SOCKET) {
      revents |= CELL_POLLOUT;
    }
  }
  return revents;
}

int cell_fd_poll_events(int fd, int events) {
  return fd_poll_events_for_domain(current_domain(), fd, events);
}

static int64_t ppoll_check(struct thread *thread, bool commit) {
  if (thread == NULL || thread->domain == NULL || thread->poll_nfds > CELL_MAX_POLL_FDS) { return -EINVAL; }
  int64_t ready = 0;
  for (uint64_t i = 0; i < thread->poll_nfds; ++i) {
    struct pollfd64 pfd;
    uint64_t addr = thread->poll_fds + i * sizeof(pfd);
    if (!vmm_copy_from_user(&thread->domain->as, &pfd, addr, sizeof(pfd))) { return -EFAULT; }

    int revents = 0;
    if (pfd.fd >= 0) { revents = fd_poll_events_for_domain(thread->domain, pfd.fd, pfd.events); }
    if (revents != 0) { ++ready; }
    if (commit) {
      pfd.revents = (int16_t)revents;
      if (!vmm_copy_to_user(&thread->domain->as, addr, &pfd, sizeof(pfd))) { return -EFAULT; }
    }
  }
  return ready;
}

static int64_t pselect_check(struct thread *thread, bool commit) {
  if (thread == NULL || thread->domain == NULL || thread->poll_nfds > 64) { return -EINVAL; }
  uint64_t read_in = 0;
  uint64_t write_in = 0;
  uint64_t read_out = 0;
  uint64_t write_out = 0;
  uint64_t except_out = 0;
  if (thread->poll_readfds != 0 &&
      !vmm_copy_from_user(&thread->domain->as, &read_in, thread->poll_readfds, sizeof(read_in))) {
    return -EFAULT;
  }
  if (thread->poll_writefds != 0 &&
      !vmm_copy_from_user(&thread->domain->as, &write_in, thread->poll_writefds, sizeof(write_in))) {
    return -EFAULT;
  }

  int64_t ready = 0;
  for (uint64_t fd = 0; fd < thread->poll_nfds; ++fd) {
    uint64_t bit = 1ull << fd;
    int events = 0;
    if ((read_in & bit) != 0) { events |= CELL_POLLIN; }
    if ((write_in & bit) != 0) { events |= CELL_POLLOUT; }
    if (events == 0) { continue; }
    int revents = fd_poll_events_for_domain(thread->domain, (int)fd, events);
    if ((revents & CELL_POLLIN) != 0 && (read_in & bit) != 0) { read_out |= bit; }
    if ((revents & CELL_POLLOUT) != 0 && (write_in & bit) != 0) { write_out |= bit; }
    if ((revents & (CELL_POLLIN | CELL_POLLOUT | CELL_POLLERR | CELL_POLLHUP | CELL_POLLNVAL)) != 0) { ++ready; }
  }

  if (commit) {
    if ((thread->poll_readfds != 0 &&
         !vmm_copy_to_user(&thread->domain->as, thread->poll_readfds, &read_out, sizeof(read_out))) ||
        (thread->poll_writefds != 0 &&
         !vmm_copy_to_user(&thread->domain->as, thread->poll_writefds, &write_out, sizeof(write_out))) ||
        (thread->poll_exceptfds != 0 &&
         !vmm_copy_to_user(&thread->domain->as, thread->poll_exceptfds, &except_out, sizeof(except_out)))) {
      return -EFAULT;
    }
  }
  return ready;
}

static void clear_poll_wait(struct thread *thread) {
  thread->poll_kind = 0;
  thread->poll_has_deadline = false;
  thread->poll_deadline_tick = 0;
  thread->poll_fds = 0;
  thread->poll_nfds = 0;
  thread->poll_readfds = 0;
  thread->poll_writefds = 0;
  thread->poll_exceptfds = 0;
}

static int64_t poll_wait_check(struct thread *thread, bool commit) {
  if (thread->poll_kind == 1) { return ppoll_check(thread, commit); }
  if (thread->poll_kind == 2) { return pselect_check(thread, commit); }
  return -EINVAL;
}

static void wake_poll_waiters(void) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = &threads[i];
    if (thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_POLL || thread->domain == NULL) { continue; }
    int64_t ready = poll_wait_check(thread, false);
    bool expired = thread->poll_has_deadline && scheduler_ticks >= thread->poll_deadline_tick;
    if (ready < 0 || ready != 0 || expired) {
      if (ready >= 0) { ready = poll_wait_check(thread, true); }
      if (ready < 0) { ready = -EFAULT; }
      thread->tf.x[0] = (uint64_t)ready;
      thread->state = THREAD_RUNNABLE;
      thread->wait_reason = WAIT_NONE;
      clear_poll_wait(thread);
    }
  }
}

static void wake_sleep_waiters(void) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = &threads[i];
    if (thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_SLEEP) { continue; }
    if (scheduler_ticks < thread->sleep_deadline_tick) { continue; }
    thread->tf.x[0] = 0;
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    thread->sleep_deadline_tick = 0;
  }
}

int cell_sleep_current(uint64_t timeout_ticks, struct trap_frame *frame) {
  if (current_thread == NULL || current_domain() == NULL) { return -EINVAL; }
  if (timeout_ticks == 0) { return 0; }
  cell_save_current(frame);
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_SLEEP;
  current_thread->sleep_deadline_tick = scheduler_ticks + timeout_ticks;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_ppoll_current(uint64_t fds, uint64_t nfds, bool has_timeout, uint64_t timeout_ticks,
                       struct trap_frame *frame) {
  if (current_thread == NULL || current_domain() == NULL || nfds > CELL_MAX_POLL_FDS) { return -EINVAL; }
  cell_save_current(frame);
  current_thread->poll_kind = 1;
  current_thread->poll_fds = fds;
  current_thread->poll_nfds = nfds;
  int64_t ready = ppoll_check(current_thread, true);
  if (ready != 0 || (has_timeout && timeout_ticks == 0) || ready < 0) {
    clear_poll_wait(current_thread);
    return (int)ready;
  }
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_POLL;
  current_thread->poll_has_deadline = has_timeout;
  current_thread->poll_deadline_tick = scheduler_ticks + timeout_ticks;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_pselect6_current(uint64_t nfds, uint64_t readfds, uint64_t writefds, uint64_t exceptfds, bool has_timeout,
                          uint64_t timeout_ticks, struct trap_frame *frame) {
  if (current_thread == NULL || current_domain() == NULL || nfds > 64) { return -EINVAL; }
  cell_save_current(frame);
  current_thread->poll_kind = 2;
  current_thread->poll_nfds = nfds;
  current_thread->poll_readfds = readfds;
  current_thread->poll_writefds = writefds;
  current_thread->poll_exceptfds = exceptfds;
  int64_t ready = pselect_check(current_thread, false);
  if (ready > 0) { ready = pselect_check(current_thread, true); }
  if (ready != 0 || (has_timeout && timeout_ticks == 0) || ready < 0) {
    if (ready == 0) { ready = pselect_check(current_thread, true); }
    clear_poll_wait(current_thread);
    return (int)ready;
  }
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_POLL;
  current_thread->poll_has_deadline = has_timeout;
  current_thread->poll_deadline_tick = scheduler_ticks + timeout_ticks;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int64_t cell_fd_pread_kernel(int fd, uint64_t off, void *buf, uint64_t len) {
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_RAMFS || file->node.is_dir || file->node.device != RAMFS_DEV_NONE) { return -22; }
  return (int64_t)vfs_read(&file->node, off, buf, len);
}

void cell_wake_stdin(void) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = &threads[i];
    if (thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_STDIN) { continue; }
    int64_t n = read_stdin_to_user(thread->domain, thread->stdin_buf, thread->stdin_len);
    if (n <= 0) { continue; }
    thread->tf.x[0] = (uint64_t)n;
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    thread->stdin_buf = 0;
    thread->stdin_len = 0;
  }
  wake_poll_waiters();
}

int64_t cell_fd_lseek(int fd, int64_t off, int whence) {
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  struct open_file *file = domain->fds[fd];
  int64_t base = 0;
  if (whence == 0) {
    base = 0;
  } else if (whence == 1) {
    base = (int64_t)file->offset;
  } else if (whence == 2) {
    struct vfs_node fresh;
    base = vfs_refresh(&file->node, &fresh) ? (int64_t)fresh.size : 0;
  } else {
    return -22;
  }
  int64_t next = base + off;
  if (next < 0) { return -22; }
  file->offset = (uint64_t)next;
  return next;
}

int cell_fd_open_node(const struct vfs_node *node, uint32_t flags) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return -12; }
  int fd = -1;
  for (int i = 0; i < MAX_FDS; ++i) {
    if (domain->fds[i] == NULL) {
      fd = i;
      break;
    }
  }
  if (fd < 0) { return -24; }
  struct open_file *file = alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_RAMFS;
  file->flags = flags;
  file->node = *node;
  if ((flags & CELL_O_APPEND) != 0) { file->offset = node->size; }
  domain->fds[fd] = file;
  return fd;
}

int cell_fd_socket_inet(uint8_t proto) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return -12; }
  int fd = -1;
  for (int i = 0; i < MAX_FDS; ++i) {
    if (domain->fds[i] == NULL) {
      fd = i;
      break;
    }
  }
  if (fd < 0) { return -24; }
  struct open_file *file = alloc_open_file();
  if (file == NULL) { return -12; }
  file->type = OPEN_SOCKET;
  file->socket_proto = proto;
  file->udp_local_port = (uint16_t)(40000 + fd);
  domain->fds[fd] = file;
  return fd;
}

static struct open_file *udp_socket_for_fd(struct domain *domain, int fd) {
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL || domain->fds[fd]->type != OPEN_SOCKET) {
    return NULL;
  }
  return domain->fds[fd];
}

bool cell_fd_udp_bind(int fd, uint16_t port) {
  struct open_file *file = udp_socket_for_fd(current_domain(), fd);
  if (file == NULL) { return false; }
  file->udp_local_port = port;
  return true;
}

bool cell_fd_udp_connect(int fd, uint32_t ip, uint16_t port) {
  struct open_file *file = udp_socket_for_fd(current_domain(), fd);
  if (file == NULL) { return false; }
  file->udp_remote_ip = ip;
  file->udp_remote_port = port;
  file->udp_connected = true;
  return true;
}

int64_t cell_fd_udp_send(int fd, uint32_t ip, uint16_t port, uint64_t buf, uint64_t len) {
  struct domain *domain = current_domain();
  struct open_file *file = udp_socket_for_fd(domain, fd);
  if (file == NULL) { return -9; }
  uint32_t effective_ip = ip;
  uint16_t effective_port = port;
  if (effective_ip == 0 && effective_port == 0 && file->udp_connected) {
    effective_ip = file->udp_remote_ip;
    effective_port = file->udp_remote_port;
  }
  if (effective_ip == 0 || (file->socket_proto == IPPROTO_UDP && effective_port == 0)) { return -EINVAL; }
  if (file->socket_proto == IPPROTO_UDP && !cell_egress_allowed(IPPROTO_UDP, effective_ip, effective_port)) {
    return -EPERM;
  }
  if (!file->udp_connected) {
    file->udp_remote_ip = effective_ip;
    file->udp_remote_port = effective_port;
    file->udp_connected = true;
  }
  uint8_t tmp[1472];
  if (len > sizeof(tmp)) { return -EMSGSIZE; }
  if (!vmm_copy_from_user(&domain->as, tmp, buf, (size_t)len)) { return -EFAULT; }
  bool sent = false;
  if (file->socket_proto == IPPROTO_UDP) {
    sent = net_udp_send(file->udp_local_port, effective_ip, effective_port, tmp, (size_t)len);
  } else if (file->socket_proto == IPPROTO_ICMP) {
    sent = net_icmp_send_echo(effective_ip, tmp, (size_t)len);
  }
  if (!sent) { return -EIO; }
  kprintf("[spore] socket send fd=%d proto=%u dst=%x:%u len=%u\n", fd, (unsigned)file->socket_proto,
          (unsigned)effective_ip, (unsigned)effective_port, (unsigned)len);
  return (int64_t)len;
}

int64_t cell_fd_socket_recv(int fd, uint64_t buf, uint64_t len, struct trap_frame *frame) {
  struct domain *domain = current_domain();
  struct open_file *file = udp_socket_for_fd(domain, fd);
  if (file == NULL) { return -9; }
  net_poll();
  if (file->udp_rx_len == 0) {
    if (frame == NULL) { return -EAGAIN; }
    cell_save_current(frame);
    current_thread->state = THREAD_BLOCKED;
    current_thread->wait_reason = WAIT_SOCKET;
    current_thread->wait_target = fd;
    cell_schedule(frame);
    return CELL_SWITCHED;
  }
  uint64_t n = file->udp_rx_len < len ? file->udp_rx_len : len;
  if (!vmm_copy_to_user(&domain->as, buf, file->udp_rx, (size_t)n)) { return -EFAULT; }
  file->udp_rx_len = 0;
  return (int64_t)n;
}

static bool socket_matches_udp(struct open_file *file, uint32_t src_ip, uint16_t src_port, uint16_t dst_port) {
  return file != NULL && file->type == OPEN_SOCKET && file->socket_proto == IPPROTO_UDP &&
         file->udp_local_port == dst_port &&
         (!file->udp_connected || (file->udp_remote_ip == src_ip && file->udp_remote_port == src_port));
}

static void wake_socket_waiters(struct open_file *file) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = &threads[i];
    if (thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_SOCKET || thread->domain == NULL) { continue; }
    int fd = thread->wait_target;
    if (fd >= 0 && fd < MAX_FDS && thread->domain->fds[fd] == file) {
      thread->state = THREAD_RUNNABLE;
      thread->wait_reason = WAIT_NONE;
      thread->wait_target = -1;
    }
  }
  wake_poll_waiters();
}

void cell_net_deliver_udp(uint32_t src_ip, uint16_t src_port, uint16_t dst_port, const void *payload, size_t len) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    if (!domains[d].used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domains[d].fds[fd];
      if (!socket_matches_udp(file, src_ip, src_port, dst_port)) { continue; }
      if (len > sizeof(file->udp_rx)) { len = sizeof(file->udp_rx); }
      kmemcpy(file->udp_rx, payload, len);
      file->udp_rx_len = len;
      file->udp_rx_ip = src_ip;
      file->udp_rx_port = src_port;
      wake_socket_waiters(file);
      return;
    }
  }
}

void cell_net_deliver_icmp(uint32_t src_ip, const void *payload, size_t len) {
  for (size_t d = 0; d < MAX_DOMAINS; ++d) {
    if (!domains[d].used) { continue; }
    for (size_t fd = 0; fd < MAX_FDS; ++fd) {
      struct open_file *file = domains[d].fds[fd];
      if (file == NULL || file->type != OPEN_SOCKET || file->socket_proto != IPPROTO_ICMP) { continue; }
      if (len > sizeof(file->udp_rx)) { len = sizeof(file->udp_rx); }
      kmemcpy(file->udp_rx, payload, len);
      file->udp_rx_len = len;
      file->udp_rx_ip = src_ip;
      wake_socket_waiters(file);
      return;
    }
  }
}

int cell_fd_dup(int oldfd, int minfd) {
  struct domain *domain = current_domain();
  if (domain == NULL || oldfd < 0 || oldfd >= MAX_FDS || minfd < 0 || minfd >= MAX_FDS || domain->fds[oldfd] == NULL) {
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
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return -9; }
  release_open_file(domain->fds[fd]);
  domain->fds[fd] = NULL;
  return 0;
}

bool cell_fd_stat(int fd, struct vfs_node *out) {
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type == OPEN_STDOUT || file->type == OPEN_STDIN) {
    *out = (struct vfs_node){
      .backend = VFS_RAMFS,
      .ino = 10,
      .is_dir = false,
      .device = RAMFS_DEV_TTY,
      .mode = 0020666u,
      .links_count = 1,
      .dev_id = 0x0005,
      .rdev = (5u << 8),
    };
    return true;
  }
  return vfs_refresh(&file->node, out);
}

bool cell_fd_is_dir(int fd) {
  struct vfs_node node;
  return cell_fd_stat(fd, &node) && node.is_dir;
}

bool cell_fd_next_dirent(int fd, struct vfs_dirent *out) {
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return false; }
  struct open_file *file = domain->fds[fd];
  if (file->type != OPEN_RAMFS || !file->node.is_dir) { return false; }
  return vfs_next_dirent(&file->node, &file->offset, out);
}

void cell_fd_rewind_one_dirent(int fd) {
  struct domain *domain = current_domain();
  if (domain != NULL && fd >= 0 && fd < MAX_FDS && domain->fds[fd] != NULL && domain->fds[fd]->offset > 0) {
    --domain->fds[fd]->offset;
  }
}

uint64_t cell_fd_dir_offset(int fd) {
  struct domain *domain = current_domain();
  if (domain == NULL || fd < 0 || fd >= MAX_FDS || domain->fds[fd] == NULL) { return 0; }
  return domain->fds[fd]->offset;
}

void cell_fd_set_dir_offset(int fd, uint64_t offset) {
  struct domain *domain = current_domain();
  if (domain != NULL && fd >= 0 && fd < MAX_FDS && domain->fds[fd] != NULL) { domain->fds[fd]->offset = offset; }
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
  if (domain == NULL) { return false; }
  uint64_t va = far & ~(uint64_t)(PAGE_SIZE - 1);
  const struct vma *vma = vma_lookup(&domain->vmas, va);
  if (vma == NULL || vma->type != VMA_ANON || !access_allowed(vma, access)) { return false; }
  return vmm_alloc_page(&domain->as, va, vma->prot);
}

bool cell_ensure_user_range(uint64_t va, size_t len, enum vmm_access access) {
  struct domain *domain = current_domain();
  if (domain == NULL || len == 0) { return true; }
  uint64_t end = va + len - 1;
  if (end < va) { return false; }
  uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
  uint64_t last = end & ~(uint64_t)(PAGE_SIZE - 1);
  for (;;) {
    if (!vmm_is_mapped(&domain->as, page) && !cell_handle_translation_fault(page, access)) { return false; }
    if (access == VMM_ACCESS_WRITE && !vmm_user_range_accessible(&domain->as, page, 1, VMM_ACCESS_WRITE) &&
        !vmm_handle_cow_fault(&domain->as, page)) {
      return false;
    }
    if (!vmm_user_range_accessible(&domain->as, page, 1, access)) { return false; }
    if (page == last) { return true; }
    page += PAGE_SIZE;
  }
}

bool cell_add_vma(uint64_t start, uint64_t end, uint32_t prot, uint32_t flags) {
  struct domain *domain = current_domain();
  return domain != NULL && vma_insert(&domain->vmas, start, end, prot, flags, VMA_ANON);
}

bool cell_remove_vma(uint64_t start, uint64_t end) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return false; }
  vmm_unmap_range(&domain->as, start, end);
  return vma_remove(&domain->vmas, start, end);
}

bool cell_protect_vma(uint64_t start, uint64_t end, uint32_t prot) {
  struct domain *domain = current_domain();
  if (domain == NULL) { return false; }
  if (!vma_protect(&domain->vmas, start, end, prot)) { return false; }
  vmm_protect_range(&domain->as, start, end, prot);
  return true;
}

size_t cell_resident_pages(uint64_t start, uint64_t end) {
  struct domain *domain = current_domain();
  return domain == NULL ? 0 : vmm_mapped_pages_in_range(&domain->as, start, end);
}

static size_t domain_resident_pages_uncached(const struct domain *domain) {
  size_t pages = 0;
  for (size_t i = 0; i < MAX_VMAS; ++i) {
    const struct vma *vma = &domain->vmas.entries[i];
    if (vma->used) { pages += vmm_mapped_pages_in_range(&domain->as, vma->start, vma->end); }
  }
  return pages;
}

static void refresh_proc_cache(void) {
  if (proc_cache_tick == scheduler_ticks) { return; }
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    proc_cache_rss[i] = domains[i].used ? domain_resident_pages_uncached(&domains[i]) : 0;
  }
  proc_cache_tick = scheduler_ticks;
}

static size_t domain_resident_pages(const struct domain *domain) {
  if (domain == NULL) { return 0; }
  refresh_proc_cache();
  return proc_cache_rss[(size_t)(domain - domains)];
}

size_t cell_proc_info(struct proc_info *out, size_t max) {
  size_t count = 0;
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    const struct domain *domain = &domains[i];
    if (!domain->used) { continue; }
    if (count < max && out != NULL) {
      struct proc_info info = {
        .pid = (uint32_t)domain->id,
        .tid = (uint32_t)(thread_for_domain((struct domain *)domain) == NULL
                            ? 0
                            : thread_for_domain((struct domain *)domain)->tid),
        .ppid = (uint32_t)domain->parent_id,
        .state = (uint32_t)(domain->zombie
                              ? THREAD_ZOMBIE
                              : (str_eq(domain_state_text(domain), "blocked") ? THREAD_BLOCKED : THREAD_RUNNABLE)),
        .wait_reason = 0,
        .resident_pages = domain_resident_pages(domain),
        .cpu_ticks = domain->cpu_ticks,
        .start_ticks = domain->start_ticks,
        .remaining_ticks = domain->budget.remaining_ticks,
        .max_ticks = domain->budget.max_ticks,
      };
      copy_cstr(info.name, sizeof(info.name), domain->name);
      copy_cstr(info.exec_path, sizeof(info.exec_path), domain->exec_path);
      copy_cstr(info.argv0, sizeof(info.argv0), domain->argv0);
      copy_cstr(info.cmdline, sizeof(info.cmdline), domain->cmdline);
      copy_cstr(info.cwd, sizeof(info.cwd), domain->cwd);
      out[count] = info;
    }
    ++count;
  }
  return count;
}

bool cell_proc_exists(int pid) {
  return find_domain(pid) != NULL;
}

uint32_t cell_proc_uid(int pid) {
  struct domain *domain = find_domain(pid);
  return domain == NULL ? 0 : domain->uid;
}

uint32_t cell_proc_gid(int pid) {
  struct domain *domain = find_domain(pid);
  return domain == NULL ? 0 : domain->gid;
}

int cell_proc_pid_at(size_t index) {
  size_t seen = 0;
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    if (!domains[i].used) { continue; }
    if (seen == index) { return domains[i].id; }
    ++seen;
  }
  return 0;
}

int snapshot_create_current(void) {
  struct domain *domain = current_domain();
  struct snapshot *snap = alloc_snapshot();
  if (domain == NULL || snap == NULL) { return -12; }
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
  if (parent == NULL || snap == NULL || child_domain == NULL) { return -12; }
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
