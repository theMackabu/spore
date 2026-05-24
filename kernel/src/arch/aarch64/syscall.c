#include "arch/aarch64/exceptions.h"

#include "arch/aarch64/regs.h"
#include "cell.h"
#include "elf/loader.h"
#include "exec/stack.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "ramfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  EC_SVC_A64 = 0x15,
  SYS_GETCWD = 17,
  SYS_DUP = 23,
  SYS_DUP3 = 24,
  SYS_FCNTL = 25,
  SYS_IOCTL = 29,
  SYS_MKDIRAT = 34,
  SYS_UNLINKAT = 35,
  SYS_RENAMEAT = 38,
  SYS_FTRUNCATE = 46,
  SYS_CHDIR = 49,
  SYS_FCHDIR = 50,
  SYS_OPENAT = 56,
  SYS_CLOSE = 57,
  SYS_GETDENTS64 = 61,
  SYS_LSEEK = 62,
  SYS_READ = 63,
  SYS_WRITE = 64,
  SYS_READV = 65,
  SYS_WRITEV = 66,
  SYS_READLINKAT = 78,
  SYS_NEWFSTATAT = 79,
  SYS_FSTAT = 80,
  SYS_FSYNC = 82,
  SYS_EXIT = 93,
  SYS_EXIT_GROUP = 94,
  SYS_SET_TID_ADDRESS = 96,
  SYS_FUTEX = 98,
  SYS_SET_ROBUST_LIST = 99,
  SYS_NANOSLEEP = 101,
  SYS_CLOCK_GETTIME = 113,
  SYS_CLOCK_NANOSLEEP = 115,
  SYS_SCHED_GETAFFINITY = 123,
  SYS_SCHED_YIELD = 124,
  SYS_KILL = 129,
  SYS_TGKILL = 131,
  SYS_RT_SIGACTION = 134,
  SYS_RT_SIGPROCMASK = 135,
  SYS_UNAME = 160,
  SYS_GETPID = 172,
  SYS_GETPPID = 173,
  SYS_GETUID = 174,
  SYS_GETEUID = 175,
  SYS_GETGID = 176,
  SYS_GETEGID = 177,
  SYS_GETTID = 178,
  SYS_SYSINFO = 179,
  SYS_SOCKET = 198,
  SYS_BIND = 200,
  SYS_LISTEN = 201,
  SYS_ACCEPT = 202,
  SYS_CONNECT = 203,
  SYS_GETSOCKNAME = 204,
  SYS_SENDTO = 206,
  SYS_RECVFROM = 207,
  SYS_SETSOCKOPT = 208,
  SYS_BRK = 214,
  SYS_MUNMAP = 215,
  SYS_MREMAP = 216,
  SYS_CLONE = 220,
  SYS_EXECVE = 221,
  SYS_MMAP = 222,
  SYS_MPROTECT = 226,
  SYS_MADVISE = 233,
  SYS_WAIT4 = 260,
  SYS_PRLIMIT64 = 261,
  SYS_RENAMEAT2 = 276,
  SYS_GETRANDOM = 278,
  SYS_CLONE3 = 435,
  SYS_SPORE_SNAPSHOT = 0x4000,
  SYS_SPORE_SPAWN = 0x4001,
  SYS_SPORE_REAP = 0x4002,
  SYS_SPORE_RESIDENT = 0x4003,
  SYS_SPORE_SET_BUDGET = 0x4004,
  SYS_SPORE_APPLY_POLICY = 0x4005,
  MAP_PRIVATE = 0x02,
  MAP_FIXED = 0x10,
  MAP_ANONYMOUS = 0x20,
  MREMAP_MAYMOVE = 1,
  CLONE_VM = 0x00000100,
  CLONE_FS = 0x00000200,
  CLONE_FILES = 0x00000400,
  CLONE_SIGHAND = 0x00000800,
  CLONE_THREAD = 0x00010000,
  CLONE_SYSVSEM = 0x00040000,
  CLONE_SETTLS = 0x00080000,
  CLONE_PARENT_SETTID = 0x00100000,
  CLONE_CHILD_CLEARTID = 0x00200000,
  CLONE_DETACHED = 0x00400000,
  CLONE_CHILD_SETTID = 0x01000000,
  PROT_READ = 0x1,
  PROT_WRITE = 0x2,
  PROT_EXEC = 0x4,
  MADV_DONTNEED = 4,
  MADV_FREE = 8,
  AT_FDCWD = -100,
  O_ACCMODE = 3,
  O_WRONLY = 1,
  O_RDWR = 2,
  O_CREAT = 0100,
  O_TRUNC = 01000,
  O_APPEND = 02000,
  F_DUPFD = 0,
  F_GETFL = 3,
  F_SETFL = 4,
  FUTEX_WAIT = 0,
  FUTEX_WAKE = 1,
  FUTEX_PRIVATE_FLAG = 128,
  FUTEX_CMD_MASK = 127,
  DT_REG = 8,
  DT_DIR = 4,
  AF_INET = 2,
  SOCK_DGRAM = 2,
  IPPROTO_UDP = 17,
  EROFS = 30,
  ENOENT = 2,
  EPERM = 1,
  EBADF = 9,
  ENOMEM = 12,
  EFAULT = 14,
  EINVAL = 22,
  ENOTDIR = 20,
  ENOSYS = 38,
  ENAMETOOLONG = 36,
  ENOTEMPTY = 39,
  EAFNOSUPPORT = 97,
  EPROTONOSUPPORT = 93,
  MAX_IOVCNT = 1024,
  MAX_EXEC_ARGS = 8,
  MAX_EXEC_ENVS = 8,
  MAX_EXEC_STRING = 128,
};

#define SYSCALL_SWITCHED ((int64_t)CELL_SWITCHED)

struct iovec64 {
  uint64_t base;
  uint64_t len;
};

struct timespec64 {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct stat64_aarch64 {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  int64_t st_atime_sec;
  int64_t st_atime_nsec;
  int64_t st_mtime_sec;
  int64_t st_mtime_nsec;
  int64_t st_ctime_sec;
  int64_t st_ctime_nsec;
  uint32_t __unused[2];
};

struct linux_dirent64_header {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
} __attribute__((packed));

struct sockaddr_in64 {
  uint16_t sin_family;
  uint16_t sin_port;
  uint32_t sin_addr;
  uint8_t sin_zero[8];
};

static struct user_address_space *current_as;
static struct ramfs *sys_ramfs;
static uint64_t rng_state = 0x73706f72652d7630ull;
static bool path_policy_denied;

static uint64_t align_down(uint64_t value, uint64_t align) {
  return value & ~(align - 1);
}

static uint64_t align_up(uint64_t value, uint64_t align) {
  return (value + align - 1) & ~(align - 1);
}

static bool checked_add(uint64_t a, uint64_t b, uint64_t *out) {
  *out = a + b;
  return *out >= a;
}

static bool streq(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (*a++ != *b++) { return false; }
  }
  return *a == '\0' && *b == '\0';
}

void syscall_set_address_space(struct user_address_space *as) {
  current_as = as;
}

void syscall_set_ramfs(struct ramfs *fs) {
  sys_ramfs = fs;
}

static struct user_address_space *active_as(void) {
  struct user_address_space *as = cell_current_as();
  return as == NULL ? current_as : as;
}

static uint32_t prot_to_vmm(uint64_t prot) {
  uint32_t out = 0;
  if ((prot & PROT_READ) != 0) { out |= VMM_USER_READ; }
  if ((prot & PROT_WRITE) != 0) { out |= VMM_USER_WRITE; }
  if ((prot & PROT_EXEC) != 0) { out |= VMM_USER_EXEC; }
  return out;
}

static bool user_readable(uint64_t buf, uint64_t len) {
  return cell_ensure_user_range(buf, (size_t)len, VMM_ACCESS_READ) &&
         vmm_user_range_accessible(active_as(), buf, (size_t)len, VMM_ACCESS_READ);
}

static bool user_writable(uint64_t buf, uint64_t len) {
  return cell_ensure_user_range(buf, (size_t)len, VMM_ACCESS_WRITE) &&
         vmm_user_range_accessible(active_as(), buf, (size_t)len, VMM_ACCESS_WRITE);
}

static bool copy_string_from_user(uint64_t user, char *dst, size_t cap) {
  if (cap == 0) { return false; }
  for (size_t i = 0; i + 1 < cap; ++i) {
    if (!user_readable(user + i, 1) || !vmm_copy_from_user(active_as(), &dst[i], user + i, 1)) { return false; }
    if (dst[i] == '\0') { return true; }
  }
  dst[cap - 1] = '\0';
  return false;
}

static bool normalize_path(const char *base, const char *path, char *out, size_t cap) {
  char input[256];
  size_t pos = 0;
  if (path[0] != '/') {
    for (size_t i = 0; base[i] != '\0'; ++i) {
      if (pos + 1 >= sizeof(input)) { return false; }
      input[pos++] = base[i];
    }
    if (pos == 0 || input[pos - 1] != '/') {
      if (pos + 1 >= sizeof(input)) { return false; }
      input[pos++] = '/';
    }
  }
  for (size_t i = 0; path[i] != '\0'; ++i) {
    if (pos + 1 >= sizeof(input)) { return false; }
    input[pos++] = path[i];
  }
  input[pos] = '\0';

  char components[16][32];
  size_t count = 0;
  const char *p = input;
  while (*p != '\0') {
    while (*p == '/') {
      ++p;
    }
    if (*p == '\0') { break; }
    char comp[32];
    size_t len = 0;
    while (p[len] != '\0' && p[len] != '/') {
      if (len + 1 >= sizeof(comp)) { return false; }
      comp[len] = p[len];
      ++len;
    }
    comp[len] = '\0';
    if (streq(comp, ".")) {
    } else if (streq(comp, "..")) {
      if (count > 0) { --count; }
    } else {
      if (count >= 16) { return false; }
      kmemcpy(components[count++], comp, len + 1);
    }
    p += len;
  }

  size_t out_pos = 0;
  out[out_pos++] = '/';
  for (size_t i = 0; i < count; ++i) {
    size_t len = kstrlen(components[i]);
    if (out_pos + len + (i + 1 < count ? 1 : 0) >= cap) { return false; }
    kmemcpy(out + out_pos, components[i], len);
    out_pos += len;
    if (i + 1 < count) { out[out_pos++] = '/'; }
  }
  out[out_pos] = '\0';
  return true;
}

static bool copy_resolved_path(uint64_t path_addr, char *out, size_t cap) {
  path_policy_denied = false;
  char raw[128];
  if (!copy_string_from_user(path_addr, raw, sizeof(raw)) || !normalize_path(cell_current_cwd(), raw, out, cap)) {
    return false;
  }
  const char *root = cell_current_fs_root();
  if (!streq(root, "/")) {
    size_t root_len = kstrlen(root);
    if (kmemcmp(out, root, root_len) != 0 || (out[root_len] != '\0' && out[root_len] != '/')) {
      path_policy_denied = true;
      return false;
    }
  }
  return true;
}

static bool copy_exec_path(uint64_t path_addr, char *out, size_t cap) {
  char raw[128];
  return copy_string_from_user(path_addr, raw, sizeof(raw)) && normalize_path(cell_current_cwd(), raw, out, cap);
}

static bool copy_string_array_from_user(uint64_t user, char store[][MAX_EXEC_STRING], const char *ptrs[],
                                        uint64_t max_count, uint64_t *out_count) {
  *out_count = 0;
  if (user == 0) { return true; }
  for (uint64_t i = 0; i < max_count; ++i) {
    uint64_t ptr;
    if (!user_readable(user + i * sizeof(uint64_t), sizeof(ptr)) ||
        !vmm_copy_from_user(active_as(), &ptr, user + i * sizeof(uint64_t), sizeof(ptr))) {
      return false;
    }
    if (ptr == 0) {
      *out_count = i;
      return true;
    }
    if (!copy_string_from_user(ptr, store[i], MAX_EXEC_STRING)) { return false; }
    ptrs[i] = store[i];
  }
  *out_count = max_count;
  return true;
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
  if (!user_readable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_write((int)fd, buf, len);
}

static int64_t sys_read(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len) {
  if (!user_writable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_read((int)fd, buf, len, frame);
}

static int64_t sys_writev(uint64_t fd, uint64_t iov, uint64_t iovcnt) {
  if (iovcnt > MAX_IOVCNT) { return -(int64_t)EINVAL; }
  uint64_t iov_bytes;
  if (!checked_add(0, iovcnt * sizeof(struct iovec64), &iov_bytes) ||
      (iovcnt != 0 && iov_bytes / sizeof(struct iovec64) != iovcnt) || !user_readable(iov, iov_bytes)) {
    return -(int64_t)EFAULT;
  }

  int64_t total = 0;
  for (uint64_t i = 0; i < iovcnt; ++i) {
    struct iovec64 v;
    if (!vmm_copy_from_user(active_as(), &v, iov + i * sizeof(v), sizeof(v))) { return -(int64_t)EFAULT; }
    int64_t wrote = sys_write(fd, v.base, v.len);
    if (wrote < 0) { return wrote; }
    total += wrote;
  }
  return total;
}

static int64_t sys_readv(uint64_t fd, uint64_t iov, uint64_t iovcnt) {
  if (iovcnt > MAX_IOVCNT) { return -(int64_t)EINVAL; }
  uint64_t iov_bytes = iovcnt * sizeof(struct iovec64);
  if ((iovcnt != 0 && iov_bytes / sizeof(struct iovec64) != iovcnt) || !user_readable(iov, iov_bytes)) {
    return -(int64_t)EFAULT;
  }
  int64_t total = 0;
  for (uint64_t i = 0; i < iovcnt; ++i) {
    struct iovec64 v;
    if (!vmm_copy_from_user(active_as(), &v, iov + i * sizeof(v), sizeof(v))) { return -(int64_t)EFAULT; }
    int64_t got = sys_read(NULL, fd, v.base, v.len);
    if (got < 0) { return total == 0 ? got : total; }
    total += got;
    if ((uint64_t)got != v.len) { break; }
  }
  return total;
}

static int64_t sys_brk(uint64_t requested) {
  if (requested == 0 || requested < active_as()->brk_base) { return (int64_t)active_as()->brk_current; }
  uint64_t old_end = align_up(active_as()->brk_current, PAGE_SIZE);
  uint64_t new_end = align_up(requested, PAGE_SIZE);
  if (new_end < requested) { return (int64_t)active_as()->brk_current; }
  for (uint64_t va = old_end; va < new_end; va += PAGE_SIZE) {
    if (!vmm_alloc_page(active_as(), va, VMM_USER_READ | VMM_USER_WRITE)) { return (int64_t)active_as()->brk_current; }
  }
  active_as()->brk_current = requested;
  return (int64_t)requested;
}

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags) {
  if (len == 0 || (flags & (MAP_PRIVATE | MAP_ANONYMOUS)) != (MAP_PRIVATE | MAP_ANONYMOUS)) { return -(int64_t)EINVAL; }
  uint64_t base = addr != 0 ? align_down(addr, PAGE_SIZE) : active_as()->mmap_base;
  uint64_t raw_end;
  if (!checked_add(base, len, &raw_end)) { return -(int64_t)EINVAL; }
  uint64_t end = align_up(raw_end, PAGE_SIZE);
  if (end < raw_end) { return -(int64_t)EINVAL; }
  if (!cell_mmap_allowed((end - base) / PAGE_SIZE)) { return -(int64_t)ENOMEM; }
  if ((flags & MAP_FIXED) != 0) { (void)cell_remove_vma(base, end); }
  if (!cell_add_vma(base, end, prot_to_vmm(prot), (uint32_t)flags)) { return -(int64_t)EINVAL; }
  if (addr == 0) { active_as()->mmap_base = end; }
  return (int64_t)base;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len) {
  if (len == 0) { return -(int64_t)EINVAL; }
  uint64_t start = align_down(addr, PAGE_SIZE);
  uint64_t end = align_up(addr + len, PAGE_SIZE);
  return cell_remove_vma(start, end) ? 0 : -(int64_t)EINVAL;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
  if (len == 0) { return 0; }
  uint64_t start = align_down(addr, PAGE_SIZE);
  uint64_t end = align_up(addr + len, PAGE_SIZE);
  return cell_protect_vma(start, end, prot_to_vmm(prot)) ? 0 : -(int64_t)EINVAL;
}

static int64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice) {
  if (advice == MADV_DONTNEED || advice == MADV_FREE) {
    vmm_unmap_range(active_as(), align_down(addr, PAGE_SIZE), align_up(addr + len, PAGE_SIZE));
  }
  return 0;
}

static int64_t sys_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len, uint64_t flags) {
  if (new_len <= old_len) {
    (void)sys_munmap(old_addr + new_len, old_len - new_len);
    return (int64_t)old_addr;
  }
  if ((flags & MREMAP_MAYMOVE) == 0) { return -(int64_t)EINVAL; }
  return sys_mmap(0, new_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
}

static int64_t sys_getrandom(uint64_t buf, uint64_t len) {
  if (!user_writable(buf, len)) { return -(int64_t)EFAULT; }
  for (uint64_t i = 0; i < len; ++i) {
    rng_state = rng_state * 6364136223846793005ull + 1;
    uint8_t byte = (uint8_t)(rng_state >> 32);
    if (!vmm_copy_to_user(active_as(), buf + i, &byte, 1)) { return -(int64_t)EFAULT; }
  }
  return (int64_t)len;
}

static int64_t sys_clock_gettime(uint64_t clk_id, uint64_t tp) {
  (void)clk_id;
  uint64_t cnt;
  uint64_t freq;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  struct timespec64 ts = {
    .tv_sec = (int64_t)(cnt / freq),
    .tv_nsec = (int64_t)(((cnt % freq) * 1000000000ull) / freq),
  };
  return user_writable(tp, sizeof(ts)) && vmm_copy_to_user(active_as(), tp, &ts, sizeof(ts)) ? 0 : -(int64_t)EFAULT;
}

static int64_t sys_clone(struct trap_frame *f, uint64_t flags, uint64_t newsp, uint64_t parent_tid, uint64_t tls,
                         uint64_t child_tid) {
  if ((flags & CLONE_VM) == 0) {
    if (newsp != 0) { return -(int64_t)ENOSYS; }
    return cell_fork_current(f);
  }

  const uint64_t allowed_thread_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
                                        CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID |
                                        CLONE_CHILD_SETTID | CLONE_DETACHED;
  const uint64_t signal_mask = 0xff;
  if ((flags & ~(allowed_thread_flags | signal_mask)) != 0 || newsp == 0) { return -(int64_t)ENOSYS; }
  return cell_clone_thread_current(f, flags, newsp, parent_tid, tls, child_tid);
}

static int64_t sys_futex(struct trap_frame *f, uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout) {
  uint64_t cmd = op & FUTEX_CMD_MASK;
  if ((op & ~(uint64_t)(FUTEX_CMD_MASK | FUTEX_PRIVATE_FLAG)) != 0) {
    kprintf("[kernel] futex op %d ENOSYS\n", (int)op);
    return -(int64_t)ENOSYS;
  }
  switch (cmd) {
  case FUTEX_WAIT:
    if (timeout != 0) {
      kprintf("[kernel] futex wait timeout ENOSYS\n");
      return -(int64_t)ENOSYS;
    }
    return cell_futex_wait_current(uaddr, (uint32_t)val, f);
  case FUTEX_WAKE:
    return cell_futex_wake_current(uaddr, (uint32_t)val);
  default:
    kprintf("[kernel] futex op %d ENOSYS\n", (int)op);
    return -(int64_t)ENOSYS;
  }
}

static int64_t sys_execve(struct trap_frame *frame, uint64_t path_addr, uint64_t argv_addr, uint64_t envp_addr) {
  char path[128];
  if (!copy_exec_path(path_addr, path, sizeof(path))) { return -(int64_t)EFAULT; }

  char argv_store[MAX_EXEC_ARGS][MAX_EXEC_STRING];
  char env_store[MAX_EXEC_ENVS][MAX_EXEC_STRING];
  const char *argv[MAX_EXEC_ARGS];
  const char *envp[MAX_EXEC_ENVS];
  uint64_t argc = 0;
  uint64_t envc = 0;
  if (!copy_string_array_from_user(argv_addr, argv_store, argv, MAX_EXEC_ARGS, &argc) ||
      !copy_string_array_from_user(envp_addr, env_store, envp, MAX_EXEC_ENVS, &envc)) {
    return -(int64_t)EFAULT;
  }
  if (argc == 0) {
    argv_store[0][0] = '\0';
    for (size_t i = 0; i + 1 < sizeof(argv_store[0]) && path[i] != '\0'; ++i) {
      argv_store[0][i] = path[i];
      argv_store[0][i + 1] = '\0';
    }
    argv[0] = argv_store[0];
    argc = 1;
  }

  struct ramfs_file file;
  if (!ramfs_lookup(sys_ramfs, path, &file)) { return -(int64_t)ENOENT; }

  struct user_address_space new_as;
  struct loaded_elf elf;
  struct vma_list new_vmas;
  uint64_t user_sp = 0;
  uint64_t hhdm_offset = active_as()->hhdm_offset;
  vma_list_init(&new_vmas);
  if (!vmm_user_init(&new_as, hhdm_offset)) { return -12; }
  if (!elf_load_static_aarch64(&new_as, file.data, (size_t)file.size, &elf) ||
      !build_initial_stack_args(&new_as, &elf, argv, argc, envp, envc, &user_sp)) {
    vmm_destroy(&new_as);
    return -(int64_t)EINVAL;
  }
  kprintf("[spore] execve %s\n", path);
  return cell_exec_replace(&new_as, &new_vmas, elf.entry, user_sp, frame) ? 0 : -12;
}

static int64_t sys_openat(uint64_t dirfd, uint64_t path_addr, uint64_t flags) {
  if ((int64_t)dirfd != AT_FDCWD) { return -(int64_t)EINVAL; }
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  struct ramfs_node node;
  if (!ramfs_lookup_node(sys_ramfs, path, &node)) {
    if ((flags & O_CREAT) == 0 || !ramfs_create(sys_ramfs, path, &node)) { return -(int64_t)ENOENT; }
  }
  if ((flags & O_TRUNC) != 0 && !node.is_dir) { (void)ramfs_truncate(sys_ramfs, node.index, 0); }
  return cell_fd_open_node(&node, (uint32_t)flags);
}

static void fill_stat(struct stat64_aarch64 *st, const struct ramfs_node *node) {
  kmemset(st, 0, sizeof(*st));
  st->st_ino = node->ino;
  st->st_mode = (node->is_dir ? 0040000u : 0100000u) | 0444u;
  st->st_nlink = 1;
  st->st_size = (int64_t)node->size;
  st->st_blksize = PAGE_SIZE;
  st->st_blocks = (int64_t)((node->size + 511) / 512);
}

static int64_t sys_fstat(uint64_t fd, uint64_t stat_addr) {
  struct ramfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  struct stat64_aarch64 st;
  fill_stat(&st, &node);
  return user_writable(stat_addr, sizeof(st)) && vmm_copy_to_user(active_as(), stat_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

static int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr, uint64_t stat_addr) {
  (void)dirfd;
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  struct ramfs_node node;
  if (!ramfs_lookup_node(sys_ramfs, path, &node)) { return -(int64_t)ENOENT; }
  struct stat64_aarch64 st;
  fill_stat(&st, &node);
  return user_writable(stat_addr, sizeof(st)) && vmm_copy_to_user(active_as(), stat_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

static uint16_t dirent_reclen(size_t name_len) {
  return (uint16_t)((sizeof(struct linux_dirent64_header) + name_len + 1 + 7) & ~7ull);
}

static int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len) {
  uint64_t written = 0;
  for (;;) {
    struct ramfs_dirent ent;
    if (!cell_fd_next_dirent((int)fd, &ent)) { break; }
    size_t name_len = kstrlen(ent.name);
    uint16_t reclen = dirent_reclen(name_len);
    if (written + reclen > len) {
      cell_fd_rewind_one_dirent((int)fd);
      break;
    }
    if (!user_writable(buf + written, reclen)) { return -(int64_t)EFAULT; }
    struct linux_dirent64_header hdr = {
      .d_ino = ent.ino,
      .d_off = (int64_t)cell_fd_dir_offset((int)fd),
      .d_reclen = reclen,
      .d_type = ent.is_dir ? DT_DIR : DT_REG,
    };
    if (!vmm_copy_to_user(active_as(), buf + written, &hdr, sizeof(hdr)) ||
        !vmm_copy_to_user(active_as(), buf + written + sizeof(hdr), ent.name, name_len + 1)) {
      return -(int64_t)EFAULT;
    }
    written += reclen;
  }
  return (int64_t)written;
}

static int64_t sys_getcwd(uint64_t buf, uint64_t len) {
  const char *cwd = cell_current_cwd();
  size_t need = kstrlen(cwd) + 1;
  if (len < need || !user_writable(buf, need)) { return -(int64_t)EFAULT; }
  return vmm_copy_to_user(active_as(), buf, cwd, need) ? (int64_t)buf : -(int64_t)EFAULT;
}

static int64_t sys_chdir(uint64_t path_addr) {
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  struct ramfs_node node;
  if (!ramfs_lookup_node(sys_ramfs, path, &node)) { return -(int64_t)ENOENT; }
  if (!node.is_dir) { return -(int64_t)ENOTDIR; }
  return cell_set_cwd(path) ? 0 : -(int64_t)ENAMETOOLONG;
}

static int64_t sys_mkdirat(uint64_t dirfd, uint64_t path_addr) {
  if ((int64_t)dirfd != AT_FDCWD) { return -(int64_t)EINVAL; }
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  return ramfs_mkdir(sys_ramfs, path) ? 0 : -(int64_t)EINVAL;
}

static int64_t sys_unlinkat(uint64_t dirfd, uint64_t path_addr) {
  if ((int64_t)dirfd != AT_FDCWD) { return -(int64_t)EINVAL; }
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  return ramfs_unlink(sys_ramfs, path) ? 0 : -(int64_t)ENOTEMPTY;
}

static int64_t sys_renameat(uint64_t old_dirfd, uint64_t old_path_addr, uint64_t new_dirfd, uint64_t new_path_addr) {
  if ((int64_t)old_dirfd != AT_FDCWD || (int64_t)new_dirfd != AT_FDCWD) { return -(int64_t)EINVAL; }
  char old_path[128];
  char new_path[128];
  if (!copy_resolved_path(old_path_addr, old_path, sizeof(old_path)) ||
      !copy_resolved_path(new_path_addr, new_path, sizeof(new_path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  return ramfs_rename(sys_ramfs, old_path, new_path) ? 0 : -(int64_t)ENOENT;
}

static int64_t sys_ftruncate(uint64_t fd, uint64_t size) {
  struct ramfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  return ramfs_truncate(node.fs, node.index, size) ? 0 : -(int64_t)EINVAL;
}

static uint16_t bswap16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

static bool copy_sockaddr_in(uint64_t addr, uint64_t len, struct sockaddr_in64 *out) {
  if (addr == 0 || len < sizeof(*out) || !user_readable(addr, sizeof(*out))) { return false; }
  return vmm_copy_from_user(active_as(), out, addr, sizeof(*out)) && out->sin_family == AF_INET;
}

static int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
  if (domain != AF_INET) { return -(int64_t)EAFNOSUPPORT; }
  if ((type & 0xf) != SOCK_DGRAM || (protocol != 0 && protocol != IPPROTO_UDP)) { return -(int64_t)EPROTONOSUPPORT; }
  return cell_fd_socket_udp();
}

static int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len) {
  struct sockaddr_in64 sa;
  if (!copy_sockaddr_in(addr, len, &sa)) { return -(int64_t)EINVAL; }
  return cell_fd_udp_bind((int)fd, bswap16(sa.sin_port)) ? 0 : -(int64_t)EBADF;
}

static int64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len) {
  struct sockaddr_in64 sa;
  if (!copy_sockaddr_in(addr, len, &sa)) { return -(int64_t)EINVAL; }
  if (!cell_egress_allowed(IPPROTO_UDP, sa.sin_addr, bswap16(sa.sin_port))) { return -(int64_t)EPERM; }
  return cell_fd_udp_connect((int)fd, sa.sin_addr, bswap16(sa.sin_port)) ? 0 : -(int64_t)EBADF;
}

static int64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen) {
  (void)flags;
  if (!user_readable(buf, len)) { return -(int64_t)EFAULT; }
  uint32_t ip = 0;
  uint16_t port = 0;
  if (addr != 0) {
    struct sockaddr_in64 sa;
    if (!copy_sockaddr_in(addr, addrlen, &sa)) { return -(int64_t)EINVAL; }
    ip = sa.sin_addr;
    port = bswap16(sa.sin_port);
  }
  return cell_fd_udp_send((int)fd, ip, port, buf, len);
}

static int64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags, uint64_t addr, uint64_t addrlen) {
  (void)flags;
  (void)addr;
  (void)addrlen;
  if (!user_writable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_udp_recv((int)fd, buf, len);
}

static int64_t sys_getsockname(uint64_t fd, uint64_t addr, uint64_t addrlen) {
  (void)fd;
  if (addr == 0 || addrlen == 0 || !user_writable(addrlen, sizeof(uint32_t))) { return -(int64_t)EFAULT; }
  uint32_t len = sizeof(struct sockaddr_in64);
  (void)vmm_copy_to_user(active_as(), addrlen, &len, sizeof(len));
  if (!user_writable(addr, sizeof(struct sockaddr_in64))) { return -(int64_t)EFAULT; }
  struct sockaddr_in64 sa = {.sin_family = AF_INET};
  return vmm_copy_to_user(active_as(), addr, &sa, sizeof(sa)) ? 0 : -(int64_t)EFAULT;
}

static int64_t sys_sched_getaffinity(uint64_t mask, uint64_t len) {
  uint8_t one = 1;
  if (len == 0 || !user_writable(mask, 1)) { return -(int64_t)EINVAL; }
  return vmm_copy_to_user(active_as(), mask, &one, 1) ? 1 : -(int64_t)EFAULT;
}

static int64_t zero_user(uint64_t addr, uint64_t len) {
  uint8_t zero = 0;
  for (uint64_t i = 0; i < len; ++i) {
    if (!user_writable(addr + i, 1) || !vmm_copy_to_user(active_as(), addr + i, &zero, 1)) { return -(int64_t)EFAULT; }
  }
  return 0;
}

static int64_t dispatch(struct trap_frame *f) {
  uint64_t nr = f->x[8];
  uint64_t a0 = f->x[0];
  uint64_t a1 = f->x[1];
  uint64_t a2 = f->x[2];
  uint64_t a3 = f->x[3];
  uint64_t a4 = f->x[4];

  if (!cell_syscall_allowed(nr)) {
    kprintf("[spore] syscall denied nr=%d\n", (int)nr);
    return -(int64_t)EPERM;
  }

  switch (nr) {
  case SYS_SCHED_YIELD:
    cell_schedule(f);
    return SYSCALL_SWITCHED;
  case SYS_READ:
    return sys_read(f, a0, a1, a2);
  case SYS_WRITE:
    return sys_write(a0, a1, a2);
  case SYS_READV:
    return sys_readv(a0, a1, a2);
  case SYS_WRITEV:
    return sys_writev(a0, a1, a2);
  case SYS_EXIT:
    kprintf("[kernel] exit(%d)\n", (int)a0);
    cell_exit_thread_current((int)a0, f);
    return SYSCALL_SWITCHED;
  case SYS_EXIT_GROUP:
    kprintf("[kernel] exit_group(%d)\n", (int)a0);
    cell_exit_group_current((int)a0, f);
    return SYSCALL_SWITCHED;
  case SYS_GETPID:
    return cell_current_pid();
  case SYS_GETPPID:
    return cell_current_ppid();
  case SYS_GETTID:
    return cell_current_tid();
  case SYS_GETUID:
  case SYS_GETEUID:
  case SYS_GETGID:
  case SYS_GETEGID:
    return 0;
  case SYS_CLONE:
    return sys_clone(f, a0, a1, a2, a3, a4);
  case SYS_FUTEX: {
    int64_t rc = sys_futex(f, a0, a1, a2, a3);
    return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
  }
  case SYS_EXECVE:
    return sys_execve(f, a0, a1, a2);
  case SYS_CLONE3:
    return -(int64_t)ENOSYS;
  case SYS_WAIT4: {
    int rc = cell_wait4((int)a0, a1, f);
    return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
  }
  case SYS_KILL:
  case SYS_TGKILL:
    return cell_kill((int)(nr == SYS_TGKILL ? a1 : a0), (int)(nr == SYS_TGKILL ? a2 : a1));
  case SYS_SPORE_SNAPSHOT:
    return snapshot_create_current();
  case SYS_SPORE_SPAWN:
    return snapshot_spawn((int)a0, a1, a2, f);
  case SYS_SPORE_REAP: {
    int rc = snapshot_reap((int)a0, a1, f);
    return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
  }
  case SYS_SPORE_RESIDENT:
    return (int64_t)cell_resident_pages(a0, a0 + a1);
  case SYS_SPORE_SET_BUDGET:
    return cell_set_budget((int)a0, a1);
  case SYS_SPORE_APPLY_POLICY: {
    char manifest[64];
    if (!copy_string_from_user(a0, manifest, sizeof(manifest))) { return -(int64_t)EFAULT; }
    int rc = cell_apply_policy(manifest);
    return rc == 0 ? 0 : -(int64_t)EPERM;
  }
  case SYS_BRK:
    return sys_brk(a0);
  case SYS_MMAP:
    return sys_mmap(a0, a1, a2, a3);
  case SYS_MUNMAP:
    return sys_munmap(a0, a1);
  case SYS_MPROTECT:
    return sys_mprotect(a0, a1, a2);
  case SYS_MADVISE:
    return sys_madvise(a0, a1, a2);
  case SYS_MREMAP:
    return sys_mremap(a0, a1, a2, a3);
  case SYS_SOCKET:
    return sys_socket(a0, a1, a2);
  case SYS_BIND:
    return sys_bind(a0, a1, a2);
  case SYS_CONNECT:
    return sys_connect(a0, a1, a2);
  case SYS_SENDTO:
    return sys_sendto(a0, a1, a2, a3, a4, f->x[5]);
  case SYS_RECVFROM:
    return sys_recvfrom(a0, a1, a2, a3, a4, f->x[5]);
  case SYS_GETSOCKNAME:
    return sys_getsockname(a0, a1, a2);
  case SYS_SETSOCKOPT:
    return 0;
  case SYS_LISTEN:
  case SYS_ACCEPT:
    return -(int64_t)ENOSYS;
  case SYS_OPENAT:
    return sys_openat(a0, a1, a2);
  case SYS_CLOSE:
    return cell_fd_close((int)a0);
  case SYS_LSEEK:
    return cell_fd_lseek((int)a0, (int64_t)a1, (int)a2);
  case SYS_FSTAT:
    return sys_fstat(a0, a1);
  case SYS_NEWFSTATAT:
    return sys_newfstatat(a0, a1, a2);
  case SYS_GETDENTS64:
    return sys_getdents64(a0, a1, a2);
  case SYS_READLINKAT:
    return -(int64_t)EINVAL;
  case SYS_DUP:
    return cell_fd_dup((int)a0, 0);
  case SYS_DUP3:
    return cell_fd_dup((int)a0, (int)a1);
  case SYS_FCNTL:
    if (a1 == F_DUPFD) { return cell_fd_dup((int)a0, (int)a2); }
    if (a1 == F_GETFL || a1 == F_SETFL) { return 0; }
    return -(int64_t)EINVAL;
  case SYS_GETCWD:
    return sys_getcwd(a0, a1);
  case SYS_PRLIMIT64:
    return a3 == 0 ? 0 : zero_user(a3, 128);
  case SYS_SYSINFO:
  case SYS_UNAME:
    return zero_user(a0 == 0 ? a2 : a0, 128);
  case SYS_SET_ROBUST_LIST:
    return cell_set_robust_list_current(a0);
  case SYS_RT_SIGACTION:
  case SYS_RT_SIGPROCMASK:
  case SYS_NANOSLEEP:
  case SYS_CLOCK_NANOSLEEP:
    return 0;
  case SYS_SCHED_GETAFFINITY:
    return sys_sched_getaffinity(a2, a1);
  case SYS_SET_TID_ADDRESS:
    return cell_set_tid_address_current(a0);
  case SYS_IOCTL:
    return 0;
  case SYS_MKDIRAT:
    return sys_mkdirat(a0, a1);
  case SYS_UNLINKAT:
    return sys_unlinkat(a0, a1);
  case SYS_RENAMEAT:
    return sys_renameat(a0, a1, a2, a3);
  case SYS_RENAMEAT2:
    return a4 == 0 ? sys_renameat(a0, a1, a2, a3) : -(int64_t)EINVAL;
  case SYS_FTRUNCATE:
    return sys_ftruncate(a0, a1);
  case SYS_CHDIR:
    return sys_chdir(a0);
  case SYS_FCHDIR:
    return cell_fd_is_dir((int)a0) ? 0 : -(int64_t)ENOTDIR;
  case SYS_FSYNC:
    return 0;
  case SYS_GETRANDOM:
    return sys_getrandom(a0, a1);
  case SYS_CLOCK_GETTIME:
    return sys_clock_gettime(a0, a1);
  default:
    kprintf("[kernel] syscall %d ENOSYS\n", (int)nr);
    return -(int64_t)ENOSYS;
  }
}

void handle_lower_sync(struct trap_frame *frame) {
  uint64_t ec = (frame->esr_el1 >> 26) & 0x3f;
  if (ec != EC_SVC_A64) {
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    uint64_t iss = frame->esr_el1 & 0x1ffffff;
    uint64_t dfsc = iss & 0x3f;
    bool write = (iss & (1u << 6)) != 0;
    if (ec == 0x24 && dfsc >= 0x04 && dfsc <= 0x07 &&
        cell_handle_translation_fault(far, write ? VMM_ACCESS_WRITE : VMM_ACCESS_READ)) {
      return;
    }
    if (ec == 0x24 && write && dfsc >= 0x0c && dfsc <= 0x0f && cell_handle_cow_fault(far)) { return; }
    kprintf("[kernel] lower sync fault ec=%x esr=%x elr=%p far=%p\n", (unsigned)ec, (unsigned)frame->esr_el1,
            (void *)(uintptr_t)frame->elr_el1, (void *)(uintptr_t)far);
    cell_exit_group_current(128, frame);
    return;
  }
  int64_t ret = dispatch(frame);
  if (ret != SYSCALL_SWITCHED) { frame->x[0] = (uint64_t)ret; }
}
