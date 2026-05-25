#include "arch/aarch64/exceptions.h"

#include "arch/aarch64/regs.h"
#include "cell.h"
#include "elf/loader.h"
#include "exec/stack.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "pl011.h"
#include "ramfs.h"
#include "spore_version.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  EC_SVC_A64 = 0x15,
  SYS_GETCWD = 17,
  SYS_EVENTFD2 = 19,
  SYS_EPOLL_CREATE1 = 20,
  SYS_EPOLL_CTL = 21,
  SYS_EPOLL_PWAIT = 22,
  SYS_DUP = 23,
  SYS_DUP3 = 24,
  SYS_FCNTL = 25,
  SYS_IOCTL = 29,
  SYS_MKNODAT = 33,
  SYS_MKDIRAT = 34,
  SYS_UNLINKAT = 35,
  SYS_SYMLINKAT = 36,
  SYS_LINKAT = 37,
  SYS_RENAMEAT = 38,
  SYS_STATFS = 43,
  SYS_FSTATFS = 44,
  SYS_FTRUNCATE = 46,
  SYS_FACCESSAT = 48,
  SYS_CHDIR = 49,
  SYS_FCHDIR = 50,
  SYS_CHROOT = 51,
  SYS_FCHMOD = 52,
  SYS_FCHMODAT = 53,
  SYS_FCHOWNAT = 54,
  SYS_FCHOWN = 55,
  SYS_OPENAT = 56,
  SYS_CLOSE = 57,
  SYS_PIPE2 = 59,
  SYS_GETDENTS64 = 61,
  SYS_LSEEK = 62,
  SYS_READ = 63,
  SYS_WRITE = 64,
  SYS_READV = 65,
  SYS_WRITEV = 66,
  SYS_PSELECT6 = 72,
  SYS_PPOLL = 73,
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
  SYS_CLOCK_GETRES = 114,
  SYS_CLOCK_NANOSLEEP = 115,
  SYS_SCHED_GETAFFINITY = 123,
  SYS_SCHED_YIELD = 124,
  SYS_KILL = 129,
  SYS_TKILL = 130,
  SYS_TGKILL = 131,
  SYS_SIGALTSTACK = 132,
  SYS_RT_SIGACTION = 134,
  SYS_RT_SIGPROCMASK = 135,
  SYS_RT_SIGRETURN = 139,
  SYS_SETGID = 144,
  SYS_SETUID = 146,
  SYS_SET_PGID = 154,
  SYS_GET_PGID = 155,
  SYS_GET_SID = 156,
  SYS_SETSID = 157,
  SYS_UNAME = 160,
  SYS_UMASK = 166,
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
  SYS_GETSOCKOPT = 209,
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
  SYS_STATX = 291,
  SYS_CLONE3 = 435,
  SYS_FACCESSAT2 = 439,
  SYS_SPORE_SNAPSHOT = 0x4000,
  SYS_SPORE_SPAWN = 0x4001,
  SYS_SPORE_REAP = 0x4002,
  SYS_SPORE_RESIDENT = 0x4003,
  SYS_SPORE_SET_BUDGET = 0x4004,
  SYS_SPORE_APPLY_POLICY = 0x4005,
  SYS_SPORE_SHUTDOWN = 0x4006,
  SYS_SPORE_PROCINFO = 0x4007,
  SYS_SPORE_FSINFO = 0x4008,
  SYS_SPORE_MOUNTINFO = 0x4009,
  MAP_PRIVATE = 0x02,
  MAP_FIXED = 0x10,
  MAP_ANONYMOUS = 0x20,
  MREMAP_MAYMOVE = 1,
  MREMAP_FIXED = 2,
  CLONE_VM = 0x00000100,
  CLONE_FS = 0x00000200,
  CLONE_FILES = 0x00000400,
  CLONE_SIGHAND = 0x00000800,
  CLONE_VFORK = 0x00004000,
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
  AT_EMPTY_PATH = 0x1000,
  AT_SYMLINK_NOFOLLOW = 0x100,
  AT_EACCESS = 0x200,
  F_OK = 0,
  X_OK = 1,
  W_OK = 2,
  R_OK = 4,
  O_ACCMODE = 3,
  O_WRONLY = 1,
  O_RDWR = 2,
  O_CREAT = 0100,
  O_TRUNC = 01000,
  O_APPEND = 02000,
  S_IFMT = 0170000,
  S_IFIFO = 0010000,
  F_DUPFD = 0,
  F_GETFD = 1,
  F_SETFD = 2,
  F_GETFL = 3,
  F_SETFL = 4,
  TCGETS = 0x5401,
  TCSETS = 0x5402,
  TCSETSW = 0x5403,
  TCSETSF = 0x5404,
  TIOCGWINSZ = 0x5413,
  TIOCSWINSZ = 0x5414,
  TIOCGPGRP = 0x540F,
  TIOCSPGRP = 0x5410,
  POLLIN = 0x0001,
  POLLOUT = 0x0004,
  POLLERR = 0x0008,
  POLLHUP = 0x0010,
  POLLNVAL = 0x0020,
  NCCS = 32,
  ICANON = 0000002,
  ECHO = 0000010,
  FUTEX_WAIT = 0,
  FUTEX_WAKE = 1,
  FUTEX_PRIVATE_FLAG = 128,
  FUTEX_CMD_MASK = 127,
  DT_REG = 8,
  DT_DIR = 4,
  DT_CHR = 2,
  DT_FIFO = 1,
  AF_UNIX = 1,
  AF_INET = 2,
  SOCK_STREAM = 1,
  SOCK_DGRAM = 2,
  SOL_SOCKET = 1,
  SO_ERROR = 4,
  SO_PEERCRED = 17,
  IPPROTO_ICMP = 1,
  IPPROTO_UDP = 17,
  EROFS = 30,
  ENOENT = 2,
  EPERM = 1,
  EBADF = 9,
  EACCES = 13,
  ENOMEM = 12,
  EFAULT = 14,
  EEXIST = 17,
  EINVAL = 22,
  ENOTDIR = 20,
  ENOSYS = 38,
  ENAMETOOLONG = 36,
  ENOTEMPTY = 39,
  EAFNOSUPPORT = 97,
  ENOPROTOOPT = 92,
  EPROTONOSUPPORT = 93,
  ENOTCONN = 107,
  MAX_IOVCNT = 1024,
  MAX_POLL_FDS = 64,
  MAX_EXEC_ARGS = 8,
  MAX_EXEC_ENVS = 8,
  MAX_EXEC_STRING = 128,
  EXT2_SUPER_MAGIC = 0xef53,
  TMPFS_MAGIC = 0x01021994,
  PROC_SUPER_MAGIC = 0x9fa0,
  DEVFS_SUPER_MAGIC = 0x1373,
  STATX_TYPE = 0x00000001,
  STATX_MODE = 0x00000002,
  STATX_NLINK = 0x00000004,
  STATX_UID = 0x00000008,
  STATX_GID = 0x00000010,
  STATX_ATIME = 0x00000020,
  STATX_MTIME = 0x00000040,
  STATX_CTIME = 0x00000080,
  STATX_INO = 0x00000100,
  STATX_SIZE = 0x00000200,
  STATX_BLOCKS = 0x00000400,
  STATX_BASIC_STATS = 0x000007ff,
};

#define SYSCALL_SWITCHED ((int64_t)CELL_SWITCHED)
#define INTERP_LOAD_BASE 0x0000006000000000ull

struct iovec64 {
  uint64_t base;
  uint64_t len;
};

struct timespec64 {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct pollfd64 {
  int32_t fd;
  int16_t events;
  int16_t revents;
};

struct epoll_event64 {
  uint32_t events;
  uint64_t data;
} __attribute__((packed));

struct stack_t64 {
  uint64_t ss_sp;
  int32_t ss_flags;
  uint32_t _pad;
  uint64_t ss_size;
};

struct rlimit64 {
  uint64_t rlim_cur;
  uint64_t rlim_max;
};

struct utsname64 {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
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

struct statfs64_aarch64 {
  uint64_t f_type;
  uint64_t f_bsize;
  uint64_t f_blocks;
  uint64_t f_bfree;
  uint64_t f_bavail;
  uint64_t f_files;
  uint64_t f_ffree;
  int32_t f_fsid[2];
  uint64_t f_namelen;
  uint64_t f_frsize;
  uint64_t f_flags;
  uint64_t f_spare[4];
};

struct statx_timestamp64 {
  int64_t tv_sec;
  uint32_t tv_nsec;
  int32_t __reserved;
};

struct statx64 {
  uint32_t stx_mask;
  uint32_t stx_blksize;
  uint64_t stx_attributes;
  uint32_t stx_nlink;
  uint32_t stx_uid;
  uint32_t stx_gid;
  uint16_t stx_mode;
  uint16_t __spare0;
  uint64_t stx_ino;
  uint64_t stx_size;
  uint64_t stx_blocks;
  uint64_t stx_attributes_mask;
  struct statx_timestamp64 stx_atime;
  struct statx_timestamp64 stx_btime;
  struct statx_timestamp64 stx_ctime;
  struct statx_timestamp64 stx_mtime;
  uint32_t stx_rdev_major;
  uint32_t stx_rdev_minor;
  uint32_t stx_dev_major;
  uint32_t stx_dev_minor;
  uint64_t stx_mnt_id;
  uint64_t stx_dio_mem_align;
  uint64_t stx_dio_offset_align;
  uint64_t __spare3[12];
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

struct sockaddr_un64 {
  uint16_t sun_family;
  char sun_path[108];
};

struct ucred64 {
  int32_t pid;
  uint32_t uid;
  uint32_t gid;
};

struct termios64 {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t c_line;
  uint8_t c_cc[NCCS];
  uint32_t c_ispeed;
  uint32_t c_ospeed;
};

struct winsize64 {
  uint16_t ws_row;
  uint16_t ws_col;
  uint16_t ws_xpixel;
  uint16_t ws_ypixel;
};

struct fs_info64 {
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
  uint64_t inode_count;
  uint64_t free_inodes;
};

struct mount_info64 {
  char source[32];
  char target[32];
  char fstype[16];
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
};

static struct user_address_space *current_as;
static struct ramfs *sys_ramfs;
static uint64_t rng_state = 0x73706f72652d7630ull;
static bool path_policy_denied;
static uint64_t realtime_base_sec;
static uint64_t realtime_base_cnt;
static uint64_t realtime_freq;

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

void syscall_set_boot_time(uint64_t epoch_sec) {
  realtime_base_sec = epoch_sec;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(realtime_base_cnt));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(realtime_freq));
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
  char virtual_path[128];
  if (!copy_string_from_user(path_addr, raw, sizeof(raw)) ||
      !normalize_path(cell_current_cwd(), raw, virtual_path, sizeof(virtual_path))) {
    return false;
  }
  const char *chroot = cell_current_chroot();
  if (streq(chroot, "/")) {
    if (kstrlen(virtual_path) >= cap) { return false; }
    kmemcpy(out, virtual_path, kstrlen(virtual_path) + 1);
  } else if (streq(virtual_path, "/")) {
    if (kstrlen(chroot) >= cap) { return false; }
    kmemcpy(out, chroot, kstrlen(chroot) + 1);
  } else {
    size_t root_len = kstrlen(chroot);
    size_t path_len = kstrlen(virtual_path);
    if (root_len + path_len >= cap) { return false; }
    kmemcpy(out, chroot, root_len);
    kmemcpy(out + root_len, virtual_path, path_len + 1);
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

static bool append_char(char *out, size_t cap, size_t *pos, char c) {
  if (*pos + 1 >= cap) { return false; }
  out[(*pos)++] = c;
  out[*pos] = '\0';
  return true;
}

static bool append_str(char *out, size_t cap, size_t *pos, const char *s) {
  while (*s != '\0') {
    if (!append_char(out, cap, pos, *s++)) { return false; }
  }
  return true;
}

static bool append_u64(char *out, size_t cap, size_t *pos, uint64_t value) {
  char tmp[32];
  size_t n = 0;
  if (value == 0) { tmp[n++] = '0'; }
  while (value != 0 && n < sizeof(tmp)) {
    tmp[n++] = (char)('0' + value % 10);
    value /= 10;
  }
  while (n > 0) {
    if (!append_char(out, cap, pos, tmp[--n])) { return false; }
  }
  return true;
}

static int64_t copy_resolved_path_at(uint64_t dirfd, uint64_t path_addr, char *out, size_t cap) {
  char raw[128];
  if (!copy_string_from_user(path_addr, raw, sizeof(raw))) { return -(int64_t)EFAULT; }
  if (raw[0] == '/' || (int64_t)dirfd == AT_FDCWD) {
    return copy_resolved_path(path_addr, out, cap) ? 0 : (path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT);
  }

  struct vfs_node base;
  if (!cell_fd_stat((int)dirfd, &base)) { return -(int64_t)EBADF; }
  if (!base.is_dir) { return -(int64_t)ENOTDIR; }

  char combined[128];
  size_t pos = 0;
  combined[0] = '\0';
  if (base.backend == VFS_PROC) {
    if (!append_str(combined, sizeof(combined), &pos, "/proc")) { return -(int64_t)ENAMETOOLONG; }
    if (base.proc_pid > 0) {
      if (!append_char(combined, sizeof(combined), &pos, '/') ||
          !append_u64(combined, sizeof(combined), &pos, (uint64_t)base.proc_pid)) {
        return -(int64_t)ENAMETOOLONG;
      }
    }
  } else if (base.backend == VFS_RAMFS && base.ramfs.mount == RAMFS_MOUNT_PROC && streq(base.ramfs.name, "proc")) {
    if (!append_str(combined, sizeof(combined), &pos, "/proc")) { return -(int64_t)ENAMETOOLONG; }
  } else if (base.backend == VFS_RAMFS && base.ramfs.mount == RAMFS_MOUNT_DEV && streq(base.ramfs.name, "dev")) {
    if (!append_str(combined, sizeof(combined), &pos, "/dev")) { return -(int64_t)ENAMETOOLONG; }
  } else if (base.backend == VFS_RAMFS && base.ramfs.mount == RAMFS_MOUNT_TMP && streq(base.ramfs.name, "tmp")) {
    if (!append_str(combined, sizeof(combined), &pos, "/tmp")) { return -(int64_t)ENAMETOOLONG; }
  } else {
    return -(int64_t)EINVAL;
  }

  if (raw[0] != '\0') {
    if (!append_char(combined, sizeof(combined), &pos, '/') || !append_str(combined, sizeof(combined), &pos, raw)) {
      return -(int64_t)ENAMETOOLONG;
    }
  }
  if (!normalize_path("/", combined, out, cap)) { return -(int64_t)ENAMETOOLONG; }
  return 0;
}

static bool copy_virtual_path(uint64_t path_addr, char *out, size_t cap) {
  char raw[128];
  return copy_string_from_user(path_addr, raw, sizeof(raw)) && normalize_path(cell_current_cwd(), raw, out, cap);
}

static bool copy_exec_path(uint64_t path_addr, char *out, size_t cap) {
  char raw[128];
  char virtual_path[128];
  if (!copy_string_from_user(path_addr, raw, sizeof(raw)) ||
      !normalize_path(cell_current_cwd(), raw, virtual_path, sizeof(virtual_path))) {
    return false;
  }
  const char *chroot = cell_current_chroot();
  if (streq(chroot, "/")) {
    if (kstrlen(virtual_path) >= cap) { return false; }
    kmemcpy(out, virtual_path, kstrlen(virtual_path) + 1);
    return true;
  }
  if (streq(virtual_path, "/")) {
    if (kstrlen(chroot) >= cap) { return false; }
    kmemcpy(out, chroot, kstrlen(chroot) + 1);
    return true;
  }
  size_t root_len = kstrlen(chroot);
  size_t path_len = kstrlen(virtual_path);
  if (root_len + path_len >= cap) { return false; }
  kmemcpy(out, chroot, root_len);
  kmemcpy(out + root_len, virtual_path, path_len + 1);
  return true;
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

static bool copy_kernel_string(char *dst, size_t cap, const char *src) {
  size_t len = kstrlen(src);
  if (len >= cap) { return false; }
  kmemcpy(dst, src, len + 1);
  return true;
}

static bool parse_shebang(const void *data, uint64_t size, char *interp, size_t interp_cap, char *arg, size_t arg_cap,
                          bool *has_arg) {
  const char *text = data;
  *has_arg = false;
  if (size < 3 || text[0] != '#' || text[1] != '!') { return false; }
  size_t i = 2;
  while (i < size && (text[i] == ' ' || text[i] == '\t')) {
    ++i;
  }
  size_t interp_len = 0;
  while (i + interp_len < size && text[i + interp_len] != '\n' && text[i + interp_len] != '\r' &&
         text[i + interp_len] != ' ' && text[i + interp_len] != '\t') {
    if (interp_len + 1 >= interp_cap) { return false; }
    interp[interp_len] = text[i + interp_len];
    ++interp_len;
  }
  if (interp_len == 0) { return false; }
  interp[interp_len] = '\0';
  i += interp_len;
  while (i < size && (text[i] == ' ' || text[i] == '\t')) {
    ++i;
  }
  size_t arg_len = 0;
  while (i + arg_len < size && text[i + arg_len] != '\n' && text[i + arg_len] != '\r') {
    if (arg_len + 1 >= arg_cap) { return false; }
    arg[arg_len] = text[i + arg_len];
    ++arg_len;
  }
  while (arg_len > 0 && (arg[arg_len - 1] == ' ' || arg[arg_len - 1] == '\t')) {
    --arg_len;
  }
  arg[arg_len] = '\0';
  *has_arg = arg_len != 0;
  return true;
}

static bool vfs_elf_read_at(void *ctx, uint64_t offset, void *dst, size_t len) {
  const struct vfs_node *node = ctx;
  return node != NULL && vfs_read(node, offset, dst, len) == len;
}

static bool make_elf_reader(const struct vfs_node *node, struct elf_reader *reader) {
  if (node == NULL || reader == NULL || node->is_dir || node->device != RAMFS_DEV_NONE) { return false; }
  *reader = (struct elf_reader){
    .read_at = vfs_elf_read_at,
    .ctx = (void *)node,
    .size = node->size,
  };
  return true;
}

static bool parse_shebang_node(const struct vfs_node *node, char *interp, size_t interp_cap, char *arg, size_t arg_cap,
                               bool *has_arg) {
  char head[256];
  uint64_t n = node->size < sizeof(head) ? node->size : sizeof(head);
  if (n == 0 || vfs_read(node, 0, head, n) != n) { return false; }
  return parse_shebang(head, n, interp, interp_cap, arg, arg_cap, has_arg);
}

static int64_t sys_write(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len) {
  if (!user_readable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_write((int)fd, buf, len, frame);
}

static int64_t sys_read(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len) {
  if (!user_writable(buf, len)) { return -(int64_t)EFAULT; }
  return cell_fd_read((int)fd, buf, len, frame);
}

static int64_t sys_writev(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt) {
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
    int64_t wrote = sys_write(frame, fd, v.base, v.len);
    if (wrote == CELL_SWITCHED) { return wrote; }
    if (wrote < 0) { return wrote; }
    total += wrote;
  }
  return total;
}

static int64_t sys_readv(struct trap_frame *frame, uint64_t fd, uint64_t iov, uint64_t iovcnt) {
  if (iovcnt > MAX_IOVCNT) { return -(int64_t)EINVAL; }
  uint64_t iov_bytes = iovcnt * sizeof(struct iovec64);
  if ((iovcnt != 0 && iov_bytes / sizeof(struct iovec64) != iovcnt) || !user_readable(iov, iov_bytes)) {
    return -(int64_t)EFAULT;
  }
  int64_t total = 0;
  for (uint64_t i = 0; i < iovcnt; ++i) {
    struct iovec64 v;
    if (!vmm_copy_from_user(active_as(), &v, iov + i * sizeof(v), sizeof(v))) { return -(int64_t)EFAULT; }
    int64_t got = sys_read(frame, fd, v.base, v.len);
    if (got == CELL_SWITCHED) { return got; }
    if (got < 0) { return total == 0 ? got : total; }
    total += got;
    if ((uint64_t)got != v.len) { break; }
  }
  return total;
}

static uint64_t timespec_to_scheduler_ticks(const struct timespec64 *timeout) {
  uint64_t ticks =
    (uint64_t)timeout->tv_sec * 100ull + ((uint64_t)timeout->tv_nsec * 100ull + 999999999ull) / 1000000000ull;
  return ticks == 0 ? 1 : ticks;
}

static int64_t sys_nanosleep(struct trap_frame *frame, uint64_t req_addr, uint64_t rem_addr) {
  (void)rem_addr;
  struct timespec64 req;
  if (!user_readable(req_addr, sizeof(req)) || !vmm_copy_from_user(active_as(), &req, req_addr, sizeof(req))) {
    return -(int64_t)EFAULT;
  }
  if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000) { return -(int64_t)EINVAL; }
  if (req.tv_sec == 0 && req.tv_nsec == 0) { return 0; }
  return cell_sleep_current(timespec_to_scheduler_ticks(&req), frame);
}

static int64_t sys_clock_nanosleep(struct trap_frame *frame, uint64_t clock_id, uint64_t flags, uint64_t req_addr,
                                   uint64_t rem_addr) {
  enum {
    CLOCK_REALTIME = 0,
    CLOCK_MONOTONIC = 1,
  };
  (void)rem_addr;
  if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) { return -(int64_t)EINVAL; }
  if (flags != 0) { return -(int64_t)EINVAL; }
  struct timespec64 req;
  if (!user_readable(req_addr, sizeof(req)) || !vmm_copy_from_user(active_as(), &req, req_addr, sizeof(req))) {
    return -(int64_t)EFAULT;
  }
  if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000) { return -(int64_t)EINVAL; }
  if (req.tv_sec == 0 && req.tv_nsec == 0) { return 0; }
  return cell_sleep_current(timespec_to_scheduler_ticks(&req), frame);
}

static int64_t sys_ppoll(struct trap_frame *frame, uint64_t fds, uint64_t nfds, uint64_t timeout_addr, uint64_t sigmask,
                         uint64_t sigsetsize) {
  (void)sigmask;
  (void)sigsetsize;
  if (nfds > MAX_POLL_FDS) { return -(int64_t)EINVAL; }
  uint64_t fds_bytes = nfds * sizeof(struct pollfd64);
  if ((nfds != 0 && fds_bytes / sizeof(struct pollfd64) != nfds) || !user_readable(fds, fds_bytes) ||
      !user_writable(fds, fds_bytes)) {
    return -(int64_t)EFAULT;
  }

  struct timespec64 timeout = {.tv_sec = 0, .tv_nsec = 0};
  bool has_timeout = timeout_addr != 0;
  uint64_t timeout_ticks = 0;
  if (has_timeout) {
    if (!user_readable(timeout_addr, sizeof(timeout)) ||
        !vmm_copy_from_user(active_as(), &timeout, timeout_addr, sizeof(timeout))) {
      return -(int64_t)EFAULT;
    }
    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000) { return -(int64_t)EINVAL; }
    if (timeout.tv_sec != 0 || timeout.tv_nsec != 0) { timeout_ticks = timespec_to_scheduler_ticks(&timeout); }
  }
  return cell_ppoll_current(fds, nfds, has_timeout, timeout_ticks, frame);
}

static int64_t sys_pselect6(struct trap_frame *frame, uint64_t nfds, uint64_t readfds, uint64_t writefds,
                            uint64_t exceptfds, uint64_t timeout_addr) {
  struct timespec64 timeout = {.tv_sec = 0, .tv_nsec = 0};
  bool has_timeout = timeout_addr != 0;
  uint64_t timeout_ticks = 0;
  if (has_timeout) {
    if (!user_readable(timeout_addr, sizeof(timeout)) ||
        !vmm_copy_from_user(active_as(), &timeout, timeout_addr, sizeof(timeout))) {
      return -(int64_t)EFAULT;
    }
    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000) { return -(int64_t)EINVAL; }
    if (timeout.tv_sec != 0 || timeout.tv_nsec != 0) { timeout_ticks = timespec_to_scheduler_ticks(&timeout); }
  }
  return cell_pselect6_current(nfds, readfds, writefds, exceptfds, has_timeout, timeout_ticks, frame);
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

static int64_t map_file_private(uint64_t base, uint64_t len, uint64_t prot, int fd, uint64_t off) {
  uint64_t end = align_up(base + len, PAGE_SIZE);
  uint32_t final_flags = prot_to_vmm(prot);
  uint32_t load_flags = final_flags | VMM_USER_WRITE;
  uint8_t tmp[256];

  for (uint64_t va = base; va < end; va += PAGE_SIZE) {
    if (!vmm_alloc_page(active_as(), va, load_flags)) { return -(int64_t)ENOMEM; }
  }

  uint64_t done = 0;
  while (done < len) {
    uint64_t chunk = len - done;
    if (chunk > sizeof(tmp)) { chunk = sizeof(tmp); }
    int64_t got = cell_fd_pread_kernel(fd, off + done, tmp, chunk);
    if (got < 0) { return got; }
    if (got == 0) { break; }
    if (!vmm_copy_to_user(active_as(), base + done, tmp, (size_t)got)) { return -(int64_t)EFAULT; }
    done += (uint64_t)got;
    if ((uint64_t)got < chunk) { break; }
  }
  vmm_protect_range(active_as(), base, end, final_flags);
  return 0;
}

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off) {
  bool anon = (flags & MAP_ANONYMOUS) != 0;
  if (len == 0 || (flags & MAP_PRIVATE) == 0 || (!anon && (int64_t)fd < 0)) { return -(int64_t)EINVAL; }
  uint64_t base = addr != 0 ? align_down(addr, PAGE_SIZE) : active_as()->mmap_base;
  uint64_t raw_end;
  if (!checked_add(base, len, &raw_end)) { return -(int64_t)EINVAL; }
  uint64_t end = align_up(raw_end, PAGE_SIZE);
  if (end < raw_end) { return -(int64_t)EINVAL; }
  if (!cell_mmap_allowed((end - base) / PAGE_SIZE)) { return -(int64_t)ENOMEM; }
  if ((flags & MAP_FIXED) != 0) { (void)cell_remove_vma(base, end); }
  if (!cell_add_vma(base, end, prot_to_vmm(prot), (uint32_t)flags)) { return -(int64_t)EINVAL; }
  if (!anon) {
    int64_t rc = map_file_private(base, len, prot, (int)fd, off);
    if (rc < 0) {
      (void)cell_remove_vma(base, end);
      return rc;
    }
  }
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
  uint32_t vmm_prot = prot_to_vmm(prot);
  if (cell_protect_vma(start, end, vmm_prot)) { return 0; }
  for (uint64_t va = start; va < end; va += PAGE_SIZE) {
    if (!vmm_is_mapped(active_as(), va)) { return -(int64_t)EINVAL; }
  }
  vmm_protect_range(active_as(), start, end, vmm_prot);
  return 0;
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
  uint64_t stack_start = USER_STACK_TOP - USER_STACK_SIZE;
  if ((flags & (MREMAP_MAYMOVE | MREMAP_FIXED)) == 0 && old_addr < USER_STACK_TOP && old_addr + old_len > stack_start) {
    return -(int64_t)ENOMEM;
  }
  if ((flags & MREMAP_MAYMOVE) == 0) { return -(int64_t)EINVAL; }
  return sys_mmap(0, new_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0);
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
  enum {
    CLOCK_REALTIME = 0,
    CLOCK_MONOTONIC = 1,
  };
  uint64_t cnt;
  uint64_t freq;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  uint64_t elapsed_cnt = cnt;
  uint64_t base_sec = 0;
  if (clk_id == CLOCK_REALTIME) {
    base_sec = realtime_base_sec;
    if (cnt >= realtime_base_cnt) { elapsed_cnt = cnt - realtime_base_cnt; }
    if (realtime_freq != 0) { freq = realtime_freq; }
  } else if (clk_id != CLOCK_MONOTONIC) {
    return -(int64_t)EINVAL;
  }
  struct timespec64 ts = {
    .tv_sec = (int64_t)(base_sec + elapsed_cnt / freq),
    .tv_nsec = (int64_t)(((elapsed_cnt % freq) * 1000000000ull) / freq),
  };
  return user_writable(tp, sizeof(ts)) && vmm_copy_to_user(active_as(), tp, &ts, sizeof(ts)) ? 0 : -(int64_t)EFAULT;
}

static int64_t sys_clock_getres(uint64_t clk_id, uint64_t tp) {
  enum {
    CLOCK_REALTIME = 0,
    CLOCK_MONOTONIC = 1,
  };
  if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) { return -(int64_t)EINVAL; }
  if (tp == 0) { return 0; }
  struct timespec64 ts = {.tv_sec = 0, .tv_nsec = 10000000};
  return user_writable(tp, sizeof(ts)) && vmm_copy_to_user(active_as(), tp, &ts, sizeof(ts)) ? 0 : -(int64_t)EFAULT;
}

static int64_t sys_sigaltstack(uint64_t new_addr, uint64_t old_addr) {
  enum { SS_DISABLE = 2 };
  if (old_addr != 0) {
    struct stack_t64 old = {.ss_sp = 0, .ss_flags = SS_DISABLE, .ss_size = 0};
    if (!user_writable(old_addr, sizeof(old)) || !vmm_copy_to_user(active_as(), old_addr, &old, sizeof(old))) {
      return -(int64_t)EFAULT;
    }
  }
  if (new_addr != 0) {
    struct stack_t64 ignored;
    if (!user_readable(new_addr, sizeof(ignored)) ||
        !vmm_copy_from_user(active_as(), &ignored, new_addr, sizeof(ignored))) {
      return -(int64_t)EFAULT;
    }
  }
  return 0;
}

static int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event_addr) {
  uint32_t events = 0;
  uint64_t data = 0;
  if ((int)op != 2) {
    struct epoll_event64 event;
    if (!user_readable(event_addr, sizeof(event)) ||
        !vmm_copy_from_user(active_as(), &event, event_addr, sizeof(event))) {
      return -(int64_t)EFAULT;
    }
    events = event.events;
    data = event.data;
  }
  return cell_fd_epoll_ctl((int)epfd, (int)op, (int)fd, events, data);
}

static int64_t sys_epoll_pwait(struct trap_frame *frame, uint64_t epfd, uint64_t events_addr, uint64_t maxevents,
                               uint64_t timeout_ms, uint64_t sigmask, uint64_t sigsetsize) {
  (void)sigmask;
  (void)sigsetsize;
  int rc = cell_epoll_wait_current((int)epfd, events_addr, (int)maxevents, (int)timeout_ms, frame);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
}

static int64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_limit, uint64_t old_limit) {
  enum {
    RLIMIT_STACK = 3,
    RLIMIT_NOFILE = 7,
    RLIMIT_AS = 9,
  };
  if (pid != 0 && (int)pid != cell_current_pid()) { return -(int64_t)EINVAL; }
  if (new_limit != 0) {
    struct rlimit64 ignored;
    if (!user_readable(new_limit, sizeof(ignored)) ||
        !vmm_copy_from_user(active_as(), &ignored, new_limit, sizeof(ignored))) {
      return -(int64_t)EFAULT;
    }
  }
  if (old_limit == 0) { return 0; }

  uint64_t unlimited = UINT64_MAX;
  struct rlimit64 out = {.rlim_cur = unlimited, .rlim_max = unlimited};
  if (resource == RLIMIT_STACK) {
    out.rlim_cur = USER_STACK_SIZE;
    out.rlim_max = USER_STACK_SIZE;
  } else if (resource == RLIMIT_NOFILE) {
    out.rlim_cur = MAX_FDS;
    out.rlim_max = MAX_FDS;
  } else if (resource == RLIMIT_AS) {
    out.rlim_cur = USER_STACK_TOP;
    out.rlim_max = USER_STACK_TOP;
  }
  return user_writable(old_limit, sizeof(out)) && vmm_copy_to_user(active_as(), old_limit, &out, sizeof(out))
           ? 0
           : -(int64_t)EFAULT;
}

static void uts_copy(char dst[65], const char *src) {
  size_t i = 0;
  while (i < 64 && src[i] != '\0') {
    dst[i] = src[i];
    ++i;
  }
  dst[i] = '\0';
}

static int64_t sys_uname(uint64_t buf) {
  struct utsname64 u;
  kmemset(&u, 0, sizeof(u));
  uts_copy(u.sysname, SPORE_UTS_SYSNAME);
  uts_copy(u.nodename, SPORE_UTS_NODENAME);
  uts_copy(u.release, SPORE_UTS_RELEASE);
  uts_copy(u.version, SPORE_UTS_VERSION);
  uts_copy(u.machine, SPORE_UTS_MACHINE);
  uts_copy(u.domainname, SPORE_UTS_DOMAINNAME);
  return user_writable(buf, sizeof(u)) && vmm_copy_to_user(active_as(), buf, &u, sizeof(u)) ? 0 : -(int64_t)EFAULT;
}

static int64_t sys_clone(struct trap_frame *f, uint64_t flags, uint64_t newsp, uint64_t parent_tid, uint64_t tls,
                         uint64_t child_tid) {
  const uint64_t signal_mask = 0xff;
  if ((flags & CLONE_VFORK) != 0) {
    const uint64_t allowed_vfork_flags = CLONE_VM | CLONE_VFORK;
    if ((flags & ~(allowed_vfork_flags | signal_mask)) != 0 || newsp != 0) { return -(int64_t)ENOSYS; }
    return cell_vfork_current(f);
  }
  if ((flags & CLONE_VM) == 0) {
    if (newsp != 0) { return -(int64_t)ENOSYS; }
    return cell_fork_current(f);
  }

  const uint64_t allowed_thread_flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
                                        CLONE_SYSVSEM | CLONE_SETTLS | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID |
                                        CLONE_CHILD_SETTID | CLONE_DETACHED;
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

static bool node_access_allowed(const struct vfs_node *node, uint64_t mask) {
  if (mask == F_OK || cell_current_euid() == 0) { return true; }
  uint32_t bits = node->mode & 0777u;
  if (cell_current_euid() == node->uid) {
    bits >>= 6;
  } else if (cell_current_egid() == node->gid) {
    bits >>= 3;
  }
  if ((mask & R_OK) != 0 && (bits & 04u) == 0) { return false; }
  if ((mask & W_OK) != 0 && (bits & 02u) == 0) { return false; }
  if ((mask & X_OK) != 0 && (bits & 01u) == 0) { return false; }
  return true;
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

  struct vfs_node exec_node;
  if (!vfs_lookup(path, &exec_node)) { return -(int64_t)ENOENT; }
  if (!node_access_allowed(&exec_node, X_OK)) { return -(int64_t)EACCES; }
  struct elf_reader exec_reader;
  if (!make_elf_reader(&exec_node, &exec_reader)) { return -(int64_t)ENOENT; }
  char shebang_store[MAX_EXEC_ARGS][MAX_EXEC_STRING];
  char shebang_interp[128];
  char shebang_arg[128];
  bool shebang_has_arg = false;
  if (parse_shebang_node(&exec_node, shebang_interp, sizeof(shebang_interp), shebang_arg, sizeof(shebang_arg),
                         &shebang_has_arg)) {
    struct vfs_node script_node = exec_node;
    struct vfs_node shebang_node;
    if (!vfs_lookup(shebang_interp, &shebang_node) || !make_elf_reader(&shebang_node, &exec_reader)) {
      return -(int64_t)ENOENT;
    }
    const char *old_argv[MAX_EXEC_ARGS];
    uint64_t old_argc = argc;
    for (uint64_t i = 0; i < old_argc; ++i) {
      old_argv[i] = argv[i];
    }
    uint64_t new_argc = 0;
    if (!copy_kernel_string(shebang_store[new_argc], MAX_EXEC_STRING, shebang_interp)) { return -(int64_t)EINVAL; }
    argv[new_argc] = shebang_store[new_argc];
    ++new_argc;
    if (shebang_has_arg) {
      if (new_argc >= MAX_EXEC_ARGS || !copy_kernel_string(shebang_store[new_argc], MAX_EXEC_STRING, shebang_arg)) {
        return -(int64_t)EINVAL;
      }
      argv[new_argc] = shebang_store[new_argc];
      ++new_argc;
    }
    if (new_argc >= MAX_EXEC_ARGS || !copy_kernel_string(shebang_store[new_argc], MAX_EXEC_STRING, path)) {
      return -(int64_t)EINVAL;
    }
    argv[new_argc] = shebang_store[new_argc];
    ++new_argc;
    for (uint64_t i = 1; i < old_argc && new_argc < MAX_EXEC_ARGS; ++i) {
      if (!copy_kernel_string(shebang_store[new_argc], MAX_EXEC_STRING, old_argv[i])) { return -(int64_t)EINVAL; }
      argv[new_argc] = shebang_store[new_argc];
      ++new_argc;
    }
    argc = new_argc;
    exec_node = shebang_node;
    (void)script_node;
  }
  char interp_path[128];
  bool has_interp = elf_find_interp_aarch64(&exec_reader, interp_path, sizeof(interp_path));

  struct user_address_space new_as;
  struct loaded_elf elf;
  struct vma_list new_vmas;
  uint64_t user_sp = 0;
  uint64_t hhdm_offset = active_as()->hhdm_offset;
  vma_list_init(&new_vmas);
  if (!vmm_user_init(&new_as, hhdm_offset)) { return -12; }
  if (!elf_load_aarch64(&new_as, &exec_reader, 0, &elf)) {
    vmm_destroy(&new_as);
    return -(int64_t)EINVAL;
  }
  if (has_interp) {
    struct vfs_node interp_node;
    struct elf_reader interp_reader;
    struct loaded_elf interp;
    if (!vfs_lookup(interp_path, &interp_node) || !make_elf_reader(&interp_node, &interp_reader) ||
        !elf_load_aarch64(&new_as, &interp_reader, INTERP_LOAD_BASE, &interp)) {
      vmm_destroy(&new_as);
      return -(int64_t)ENOENT;
    }
    elf.runtime_entry = interp.entry;
    elf.at_base = interp.load_base;
  }
  if (!build_initial_stack_args(&new_as, &elf, argv, argc, envp, envc, &user_sp)) {
    vmm_destroy(&new_as);
    return -(int64_t)EINVAL;
  }
  if (!vma_insert(&new_vmas, USER_STACK_TOP - USER_STACK_SIZE, USER_STACK_TOP, VMM_USER_READ | VMM_USER_WRITE, 0,
                  VMA_ANON)) {
    vmm_destroy(&new_as);
    return -(int64_t)EINVAL;
  }
  kprintf("[spore] execve %s\n", path);
  if (!cell_exec_replace(&new_as, &new_vmas, elf.runtime_entry, user_sp, frame, path, argv, argc)) { return -12; }
  cell_apply_exec_creds(exec_node.mode, exec_node.uid, exec_node.gid);
  return 0;
}

static int64_t sys_openat(uint64_t dirfd, uint64_t path_addr, uint64_t flags) {
  char path[128];
  int64_t path_rc = copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  struct vfs_node node;
  if (!vfs_lookup(path, &node)) {
    if ((flags & O_CREAT) == 0 || !vfs_create(path, &node)) { return -(int64_t)ENOENT; }
    (void)vfs_chown_node(&node, cell_current_euid(), cell_current_egid());
    (void)vfs_lookup(path, &node);
  }
  uint64_t access = 0;
  if ((flags & O_ACCMODE) == O_WRONLY) {
    access = W_OK;
  } else if ((flags & O_ACCMODE) == O_RDWR) {
    access = R_OK | W_OK;
  } else {
    access = R_OK;
  }
  if ((flags & O_TRUNC) != 0) { access |= W_OK; }
  if (!node_access_allowed(&node, access)) { return -(int64_t)EACCES; }
  if ((flags & O_TRUNC) != 0 && !node.is_dir) {
    (void)vfs_truncate(&node, 0);
    (void)vfs_lookup(path, &node);
  }
  return cell_fd_open_node(&node, (uint32_t)flags);
}

static void fill_stat(struct stat64_aarch64 *st, const struct vfs_node *node) {
  kmemset(st, 0, sizeof(*st));
  st->st_dev = node->dev_id;
  st->st_ino = node->ino;
  st->st_mode = node->mode;
  st->st_nlink = node->links_count == 0 ? 1 : node->links_count;
  st->st_uid = node->uid;
  st->st_gid = node->gid;
  st->st_rdev = node->rdev;
  st->st_size = (int64_t)node->size;
  st->st_blksize = PAGE_SIZE;
  st->st_blocks = (int64_t)((node->size + 511) / 512);
}

static uint32_t dev_major(uint64_t dev) {
  return (uint32_t)(dev >> 8);
}

static uint32_t dev_minor(uint64_t dev) {
  return (uint32_t)(dev & 0xffu);
}

static uint64_t statfs_magic_for_path(const char *path) {
  if (path[0] == '/' && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' && path[4] == 'c' &&
      (path[5] == '\0' || path[5] == '/')) {
    return PROC_SUPER_MAGIC;
  }
  if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && (path[4] == '\0' || path[4] == '/')) {
    return DEVFS_SUPER_MAGIC;
  }
  if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' && (path[4] == '\0' || path[4] == '/')) {
    return TMPFS_MAGIC;
  }
  return EXT2_SUPER_MAGIC;
}

static void fill_statfs(struct statfs64_aarch64 *st, const char *path) {
  struct vfs_fs_info info;
  kmemset(st, 0, sizeof(*st));
  if (!vfs_fs_info(&info)) {
    info.block_size = PAGE_SIZE;
    info.block_count = 0;
    info.free_blocks = 0;
    info.inode_count = 0;
    info.free_inodes = 0;
  }
  st->f_type = statfs_magic_for_path(path);
  st->f_bsize = info.block_size == 0 ? PAGE_SIZE : info.block_size;
  st->f_blocks = info.block_count;
  st->f_bfree = info.free_blocks;
  st->f_bavail = info.free_blocks;
  st->f_files = info.inode_count;
  st->f_ffree = info.free_inodes;
  st->f_namelen = 255;
  st->f_frsize = st->f_bsize;
}

static void fill_statx(struct statx64 *st, const struct vfs_node *node) {
  kmemset(st, 0, sizeof(*st));
  st->stx_mask = STATX_BASIC_STATS;
  st->stx_blksize = PAGE_SIZE;
  st->stx_nlink = node->links_count == 0 ? 1 : node->links_count;
  st->stx_uid = node->uid;
  st->stx_gid = node->gid;
  st->stx_mode = node->mode;
  st->stx_ino = node->ino;
  st->stx_size = node->size;
  st->stx_blocks = (node->size + 511) / 512;
  st->stx_rdev_major = dev_major(node->rdev);
  st->stx_rdev_minor = dev_minor(node->rdev);
  st->stx_dev_major = dev_major(node->dev_id);
  st->stx_dev_minor = dev_minor(node->dev_id);
}

static int64_t sys_fstat(uint64_t fd, uint64_t stat_addr) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  struct stat64_aarch64 st;
  fill_stat(&st, &node);
  return user_writable(stat_addr, sizeof(st)) && vmm_copy_to_user(active_as(), stat_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

static int64_t sys_statfs(uint64_t path_addr, uint64_t statfs_addr) {
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  struct vfs_node node;
  if (!vfs_lookup(path, &node)) { return -(int64_t)ENOENT; }
  struct statfs64_aarch64 st;
  fill_statfs(&st, path);
  return user_writable(statfs_addr, sizeof(st)) && vmm_copy_to_user(active_as(), statfs_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

static int64_t sys_fstatfs(uint64_t fd, uint64_t statfs_addr) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  struct statfs64_aarch64 st;
  fill_statfs(&st, node.backend == VFS_RAMFS ? "/tmp" : "/");
  return user_writable(statfs_addr, sizeof(st)) && vmm_copy_to_user(active_as(), statfs_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

static int64_t sys_statx(uint64_t dirfd, uint64_t path_addr, uint64_t flags, uint64_t mask, uint64_t statx_addr) {
  (void)mask;
  struct vfs_node node;
  if ((flags & AT_EMPTY_PATH) != 0 && path_addr != 0) {
    char empty[1];
    if (!vmm_copy_from_user(active_as(), empty, path_addr, 1)) { return -(int64_t)EFAULT; }
    if (empty[0] == '\0') {
      if ((int64_t)dirfd == AT_FDCWD) {
        char cwd[128];
        if (!normalize_path("/", cell_current_cwd(), cwd, sizeof(cwd)) || !vfs_lookup(cwd, &node)) {
          return -(int64_t)ENOENT;
        }
      } else if (!cell_fd_stat((int)dirfd, &node)) {
        return -(int64_t)EBADF;
      }
      struct statx64 st;
      fill_statx(&st, &node);
      return user_writable(statx_addr, sizeof(st)) && vmm_copy_to_user(active_as(), statx_addr, &st, sizeof(st))
               ? 0
               : -(int64_t)EFAULT;
    }
  }
  char path[128];
  int64_t path_rc = copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  bool nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
  if (!(nofollow ? vfs_lstat(path, &node) : vfs_lookup(path, &node))) { return -(int64_t)ENOENT; }
  struct statx64 st;
  fill_statx(&st, &node);
  return user_writable(statx_addr, sizeof(st)) && vmm_copy_to_user(active_as(), statx_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

static int64_t sys_newfstatat(uint64_t dirfd, uint64_t path_addr, uint64_t stat_addr, uint64_t flags) {
  char path[128];
  int64_t path_rc = copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  struct vfs_node node;
  bool nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
  if (!(nofollow ? vfs_lstat(path, &node) : vfs_lookup(path, &node))) { return -(int64_t)ENOENT; }
  struct stat64_aarch64 st;
  fill_stat(&st, &node);
  return user_writable(stat_addr, sizeof(st)) && vmm_copy_to_user(active_as(), stat_addr, &st, sizeof(st))
           ? 0
           : -(int64_t)EFAULT;
}

static int64_t sys_faccessat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t flags) {
  if ((mode & ~(uint64_t)(R_OK | W_OK | X_OK)) != 0) { return -(int64_t)EINVAL; }
  if ((flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW | AT_EACCESS | AT_EMPTY_PATH)) != 0) { return -(int64_t)EINVAL; }
  struct vfs_node node;
  if ((flags & AT_EMPTY_PATH) != 0 && path_addr != 0) {
    char empty[1];
    if (!vmm_copy_from_user(active_as(), empty, path_addr, 1)) { return -(int64_t)EFAULT; }
    if (empty[0] == '\0') {
      if ((int64_t)dirfd == AT_FDCWD) {
        char cwd[128];
        if (!normalize_path("/", cell_current_cwd(), cwd, sizeof(cwd)) || !vfs_lookup(cwd, &node)) {
          return -(int64_t)ENOENT;
        }
      } else if (!cell_fd_stat((int)dirfd, &node)) {
        return -(int64_t)EBADF;
      }
      return node_access_allowed(&node, mode) ? 0 : -(int64_t)EACCES;
    }
  }
  char path[128];
  int64_t path_rc = copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  bool nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
  if (!(nofollow ? vfs_lstat(path, &node) : vfs_lookup(path, &node))) { return -(int64_t)ENOENT; }
  return node_access_allowed(&node, mode) ? 0 : -(int64_t)EACCES;
}

static uint16_t dirent_reclen(size_t name_len) {
  return (uint16_t)((sizeof(struct linux_dirent64_header) + name_len + 1 + 7) & ~7ull);
}

static int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len) {
  uint64_t written = 0;
  for (;;) {
    struct vfs_dirent ent;
    uint64_t old_dir_offset = cell_fd_dir_offset((int)fd);
    if (!cell_fd_next_dirent((int)fd, &ent)) { break; }
    size_t name_len = kstrlen(ent.name);
    uint16_t reclen = dirent_reclen(name_len);
    if (written + reclen > len) {
      cell_fd_set_dir_offset((int)fd, old_dir_offset);
      break;
    }
    if (!user_writable(buf + written, reclen)) { return -(int64_t)EFAULT; }
    struct linux_dirent64_header hdr = {
      .d_ino = ent.ino,
      .d_off = (int64_t)cell_fd_dir_offset((int)fd),
      .d_reclen = reclen,
      .d_type = ent.type != 0 ? ent.type : (ent.is_dir ? DT_DIR : (ent.is_device ? DT_CHR : DT_REG)),
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
  char virtual_path[128];
  char path[128];
  if (!copy_virtual_path(path_addr, virtual_path, sizeof(virtual_path)) ||
      !copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  struct vfs_node node;
  if (!vfs_lookup(path, &node)) { return -(int64_t)ENOENT; }
  if (!node.is_dir) { return -(int64_t)ENOTDIR; }
  return cell_set_cwd(virtual_path) ? 0 : -(int64_t)ENAMETOOLONG;
}

static int64_t sys_chroot(uint64_t path_addr) {
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  struct vfs_node node;
  if (!vfs_lookup(path, &node)) { return -(int64_t)ENOENT; }
  if (!node.is_dir) { return -(int64_t)ENOTDIR; }
  if (!cell_set_chroot(path) || !cell_set_cwd("/")) { return -(int64_t)ENAMETOOLONG; }
  return 0;
}

static int64_t sys_mkdirat(uint64_t dirfd, uint64_t path_addr) {
  if ((int64_t)dirfd != AT_FDCWD) { return -(int64_t)EINVAL; }
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  if (!vfs_mkdir(path)) { return -(int64_t)EINVAL; }
  (void)vfs_chown(path, cell_current_euid(), cell_current_egid());
  return 0;
}

static int64_t sys_mknodat(uint64_t dirfd, uint64_t path_addr, uint64_t mode) {
  char path[128];
  int64_t path_rc = copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  if ((mode & S_IFMT) != S_IFIFO) { return -(int64_t)EINVAL; }
  struct vfs_node node;
  if (!vfs_mkfifo(path, (uint32_t)mode, &node)) { return -(int64_t)EEXIST; }
  (void)vfs_chown_node(&node, cell_current_euid(), cell_current_egid());
  return 0;
}

static int64_t sys_unlinkat(uint64_t dirfd, uint64_t path_addr) {
  if ((int64_t)dirfd != AT_FDCWD) { return -(int64_t)EINVAL; }
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  struct vfs_node node;
  if (!vfs_lstat(path, &node)) { return -(int64_t)ENOENT; }
  return vfs_unlink(path) ? 0 : -(int64_t)ENOTEMPTY;
}

static int64_t sys_renameat(uint64_t old_dirfd, uint64_t old_path_addr, uint64_t new_dirfd, uint64_t new_path_addr) {
  if ((int64_t)old_dirfd != AT_FDCWD || (int64_t)new_dirfd != AT_FDCWD) { return -(int64_t)EINVAL; }
  char old_path[128];
  char new_path[128];
  if (!copy_resolved_path(old_path_addr, old_path, sizeof(old_path)) ||
      !copy_resolved_path(new_path_addr, new_path, sizeof(new_path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  return vfs_rename(old_path, new_path) ? 0 : -(int64_t)ENOENT;
}

static int64_t sys_linkat(uint64_t old_dirfd, uint64_t old_path_addr, uint64_t new_dirfd, uint64_t new_path_addr,
                          uint64_t flags) {
  if ((int64_t)old_dirfd != AT_FDCWD || (int64_t)new_dirfd != AT_FDCWD || flags != 0) { return -(int64_t)EINVAL; }
  char old_path[128];
  char new_path[128];
  if (!copy_resolved_path(old_path_addr, old_path, sizeof(old_path)) ||
      !copy_resolved_path(new_path_addr, new_path, sizeof(new_path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  return vfs_link(old_path, new_path) ? 0 : -(int64_t)ENOENT;
}

static int64_t sys_symlinkat(uint64_t target_addr, uint64_t new_dirfd, uint64_t link_path_addr) {
  if ((int64_t)new_dirfd != AT_FDCWD) { return -(int64_t)EINVAL; }
  char target[128];
  char link_path[128];
  if (!copy_string_from_user(target_addr, target, sizeof(target)) ||
      !copy_resolved_path(link_path_addr, link_path, sizeof(link_path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  return vfs_symlink(target, link_path) ? 0 : -(int64_t)EEXIST;
}

static int64_t sys_readlinkat(uint64_t dirfd, uint64_t path_addr, uint64_t buf, uint64_t len) {
  char path[128];
  char target[128];
  size_t target_len = 0;
  int64_t path_rc = copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  if (len == 0 || !user_writable(buf, len)) { return -(int64_t)EFAULT; }
  if (!vfs_readlink(path, target, sizeof(target), &target_len)) { return -(int64_t)EINVAL; }
  if (target_len > len) { target_len = (size_t)len; }
  return vmm_copy_to_user(active_as(), buf, target, target_len) ? (int64_t)target_len : -(int64_t)EFAULT;
}

static int64_t sys_fchmodat(uint64_t dirfd, uint64_t path_addr, uint64_t mode, uint64_t flags) {
  if ((int64_t)dirfd != AT_FDCWD || (flags & ~0x100ull) != 0) { return -(int64_t)EINVAL; }
  char path[128];
  if (!copy_resolved_path(path_addr, path, sizeof(path))) {
    return path_policy_denied ? -(int64_t)EPERM : -(int64_t)EFAULT;
  }
  return vfs_chmod(path, (uint32_t)mode) ? 0 : -(int64_t)ENOENT;
}

static int64_t sys_fchmod(uint64_t fd, uint64_t mode) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  return vfs_chmod_node(&node, (uint32_t)mode) ? 0 : -(int64_t)EINVAL;
}

static bool chown_ids(const struct vfs_node *node, uint64_t uid_arg, uint64_t gid_arg, uint32_t *uid, uint32_t *gid) {
  *uid = uid_arg == 0xffffffffull ? node->uid : (uint32_t)uid_arg;
  *gid = gid_arg == 0xffffffffull ? node->gid : (uint32_t)gid_arg;
  return uid_arg <= 0xffffffffull && gid_arg <= 0xffffffffull;
}

static int64_t sys_fchownat(uint64_t dirfd, uint64_t path_addr, uint64_t uid_arg, uint64_t gid_arg, uint64_t flags) {
  if ((flags & ~AT_SYMLINK_NOFOLLOW) != 0) { return -(int64_t)EINVAL; }
  if (cell_current_euid() != 0) { return -(int64_t)EPERM; }
  char path[128];
  int64_t path_rc = copy_resolved_path_at(dirfd, path_addr, path, sizeof(path));
  if (path_rc != 0) { return path_rc; }
  struct vfs_node node;
  bool nofollow = (flags & AT_SYMLINK_NOFOLLOW) != 0;
  if (!(nofollow ? vfs_lstat(path, &node) : vfs_lookup(path, &node))) { return -(int64_t)ENOENT; }
  uint32_t uid = 0;
  uint32_t gid = 0;
  if (!chown_ids(&node, uid_arg, gid_arg, &uid, &gid)) { return -(int64_t)EINVAL; }
  return vfs_chown(path, uid, gid) ? 0 : -(int64_t)EINVAL;
}

static int64_t sys_fchown(uint64_t fd, uint64_t uid_arg, uint64_t gid_arg) {
  if (cell_current_euid() != 0) { return -(int64_t)EPERM; }
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  uint32_t uid = 0;
  uint32_t gid = 0;
  if (!chown_ids(&node, uid_arg, gid_arg, &uid, &gid)) { return -(int64_t)EINVAL; }
  return vfs_chown_node(&node, uid, gid) ? 0 : -(int64_t)EINVAL;
}

static int64_t sys_ftruncate(uint64_t fd, uint64_t size) {
  struct vfs_node node;
  if (!cell_fd_stat((int)fd, &node)) { return -(int64_t)EBADF; }
  return vfs_truncate(&node, size) ? 0 : -(int64_t)EINVAL;
}

static uint16_t bswap16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

static bool copy_sockaddr_in(uint64_t addr, uint64_t len, struct sockaddr_in64 *out) {
  if (addr == 0 || len < sizeof(*out) || !user_readable(addr, sizeof(*out))) { return false; }
  return vmm_copy_from_user(active_as(), out, addr, sizeof(*out)) && out->sin_family == AF_INET;
}

static bool copy_sockaddr_family(uint64_t addr, uint64_t len, uint16_t *family) {
  if (addr == 0 || len < sizeof(uint16_t) || !user_readable(addr, sizeof(uint16_t))) { return false; }
  return vmm_copy_from_user(active_as(), family, addr, sizeof(*family));
}

static bool copy_sockaddr_un(uint64_t addr, uint64_t len, struct sockaddr_un64 *out) {
  if (addr == 0 || len < 3 || len > sizeof(*out) || !user_readable(addr, len)) { return false; }
  kmemset(out, 0, sizeof(*out));
  if (!vmm_copy_from_user(active_as(), out, addr, (size_t)len) || out->sun_family != AF_UNIX) { return false; }
  out->sun_path[sizeof(out->sun_path) - 1] = '\0';
  return out->sun_path[0] == '/';
}

static int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
  if (domain == AF_UNIX) {
    if ((type & 0xf) != SOCK_STREAM || protocol != 0) { return -(int64_t)EPROTONOSUPPORT; }
    return cell_fd_socket_unix();
  }
  if (domain != AF_INET) { return -(int64_t)EAFNOSUPPORT; }
  if ((type & 0xf) != SOCK_DGRAM) { return -(int64_t)EPROTONOSUPPORT; }
  if (protocol == 0) { protocol = IPPROTO_UDP; }
  if (protocol != IPPROTO_UDP && protocol != IPPROTO_ICMP) { return -(int64_t)EPROTONOSUPPORT; }
  return cell_fd_socket_inet((uint8_t)protocol);
}

static int64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len) {
  uint16_t family = 0;
  if (!copy_sockaddr_family(addr, len, &family)) { return -(int64_t)EINVAL; }
  if (family == AF_UNIX) {
    struct sockaddr_un64 sa_un;
    if (!copy_sockaddr_un(addr, len, &sa_un)) { return -(int64_t)EINVAL; }
    int rc = cell_fd_unix_bind((int)fd, sa_un.sun_path);
    return rc < 0 ? (int64_t)rc : 0;
  }
  struct sockaddr_in64 sa;
  if (!copy_sockaddr_in(addr, len, &sa)) { return -(int64_t)EINVAL; }
  return cell_fd_udp_bind((int)fd, bswap16(sa.sin_port)) ? 0 : -(int64_t)EBADF;
}

static int64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len) {
  uint16_t family = 0;
  if (!copy_sockaddr_family(addr, len, &family)) { return -(int64_t)EINVAL; }
  if (family == AF_UNIX) {
    struct sockaddr_un64 sa_un;
    if (!copy_sockaddr_un(addr, len, &sa_un)) { return -(int64_t)EINVAL; }
    int rc = cell_fd_unix_connect((int)fd, sa_un.sun_path);
    return rc < 0 ? (int64_t)rc : 0;
  }
  struct sockaddr_in64 sa;
  if (!copy_sockaddr_in(addr, len, &sa)) { return -(int64_t)EINVAL; }
  if (!cell_egress_allowed(IPPROTO_UDP, sa.sin_addr, bswap16(sa.sin_port))) { return -(int64_t)EPERM; }
  return cell_fd_udp_connect((int)fd, sa.sin_addr, bswap16(sa.sin_port)) ? 0 : -(int64_t)EBADF;
}

static int64_t sys_listen(uint64_t fd, uint64_t backlog) {
  int rc = cell_fd_unix_listen((int)fd, (int)backlog);
  return rc < 0 ? (int64_t)rc : 0;
}

static int64_t sys_accept(struct trap_frame *frame, uint64_t fd, uint64_t addr, uint64_t addrlen) {
  (void)addr;
  (void)addrlen;
  int rc = cell_fd_unix_accept((int)fd, frame);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : (int64_t)rc;
}

static int64_t sys_sendto(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                          uint64_t addr, uint64_t addrlen) {
  (void)flags;
  if (!user_readable(buf, len)) { return -(int64_t)EFAULT; }
  if (addr == 0) {
    int64_t udp = cell_fd_udp_send((int)fd, 0, 0, buf, len);
    if (udp != -EBADF) { return udp; }
    int64_t rc = cell_fd_write((int)fd, buf, len, frame);
    return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
  }
  uint32_t ip = 0;
  uint16_t port = 0;
  struct sockaddr_in64 sa;
  if (!copy_sockaddr_in(addr, addrlen, &sa)) { return -(int64_t)EINVAL; }
  ip = sa.sin_addr;
  port = bswap16(sa.sin_port);
  return cell_fd_udp_send((int)fd, ip, port, buf, len);
}

static int64_t sys_recvfrom(struct trap_frame *frame, uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                            uint64_t addr, uint64_t addrlen) {
  (void)flags;
  (void)addr;
  (void)addrlen;
  if (!user_writable(buf, len)) { return -(int64_t)EFAULT; }
  int64_t rc = cell_fd_socket_recv((int)fd, buf, len, frame);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
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

static int64_t sys_getsockopt(uint64_t fd, uint64_t level, uint64_t optname, uint64_t optval, uint64_t optlen_addr) {
  if (level != SOL_SOCKET || optval == 0 || optlen_addr == 0 || !user_writable(optlen_addr, sizeof(uint32_t))) {
    return -(int64_t)EINVAL;
  }

  uint32_t optlen = 0;
  if (!vmm_copy_from_user(active_as(), &optlen, optlen_addr, sizeof(optlen))) { return -(int64_t)EFAULT; }

  if (optname == SO_ERROR) {
    if (optlen < sizeof(int32_t) || !user_writable(optval, sizeof(int32_t))) { return -(int64_t)EINVAL; }
    int32_t zero = 0;
    uint32_t out_len = sizeof(zero);
    if (!vmm_copy_to_user(active_as(), optval, &zero, sizeof(zero)) ||
        !vmm_copy_to_user(active_as(), optlen_addr, &out_len, sizeof(out_len))) {
      return -(int64_t)EFAULT;
    }
    return 0;
  }

  if (optname == SO_PEERCRED) {
    if (optlen < sizeof(struct ucred64) || !user_writable(optval, sizeof(struct ucred64))) { return -(int64_t)EINVAL; }
    struct cell_peer_cred peer;
    if (!cell_fd_unix_peer_cred((int)fd, &peer)) { return -(int64_t)ENOTCONN; }
    struct ucred64 out = {
      .pid = peer.pid,
      .uid = peer.uid,
      .gid = peer.gid,
    };
    uint32_t out_len = sizeof(out);
    if (!vmm_copy_to_user(active_as(), optval, &out, sizeof(out)) ||
        !vmm_copy_to_user(active_as(), optlen_addr, &out_len, sizeof(out_len))) {
      return -(int64_t)EFAULT;
    }
    return 0;
  }

  return -(int64_t)ENOPROTOOPT;
}

static int64_t sys_sched_getaffinity(uint64_t mask, uint64_t len) {
  uint8_t one = 1;
  if (len == 0 || !user_writable(mask, 1)) { return -(int64_t)EINVAL; }
  return vmm_copy_to_user(active_as(), mask, &one, 1) ? 1 : -(int64_t)EFAULT;
}

static int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
  (void)fd;
  if (request == TIOCGPGRP) {
    int pgrp = cell_tty_foreground_pgrp();
    return user_writable(arg, sizeof(pgrp)) && vmm_copy_to_user(active_as(), arg, &pgrp, sizeof(pgrp))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (request == TIOCSPGRP) {
    int pgrp = 0;
    if (!user_readable(arg, sizeof(pgrp)) || !vmm_copy_from_user(active_as(), &pgrp, arg, sizeof(pgrp))) {
      return -(int64_t)EFAULT;
    }
    int rc = cell_tty_set_foreground_pgrp(pgrp);
    return rc == 0 ? 0 : (int64_t)rc;
  }
  if (request == TIOCGWINSZ) {
    uint16_t rows = 0;
    uint16_t cols = 0;
    pl011_get_winsize(&rows, &cols);
    struct winsize64 ws = {
      .ws_row = rows,
      .ws_col = cols,
      .ws_xpixel = 0,
      .ws_ypixel = 0,
    };
    return user_writable(arg, sizeof(ws)) && vmm_copy_to_user(active_as(), arg, &ws, sizeof(ws)) ? 0 : -(int64_t)EFAULT;
  }
  if (request == TIOCSWINSZ) { return 0; }
  if (request == TCGETS) {
    struct termios64 tio = {
      .c_iflag = 0,
      .c_oflag = 0,
      .c_cflag = 0,
      .c_lflag = cell_tty_lflag(),
      .c_line = 0,
      .c_cc = {0},
      .c_ispeed = 38400,
      .c_ospeed = 38400,
    };
    tio.c_cc[0] = 3;
    tio.c_cc[2] = cell_tty_erase_char();
    tio.c_cc[3] = 21;
    tio.c_cc[4] = 4;
    tio.c_cc[5] = 0;
    tio.c_cc[6] = 1;
    return user_writable(arg, sizeof(tio)) && vmm_copy_to_user(active_as(), arg, &tio, sizeof(tio)) ? 0
                                                                                                    : -(int64_t)EFAULT;
  }
  if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
    struct termios64 tio;
    if (!user_readable(arg, sizeof(tio)) || !vmm_copy_from_user(active_as(), &tio, arg, sizeof(tio))) {
      return -(int64_t)EFAULT;
    }
    cell_tty_set_lflag(tio.c_lflag);
    cell_tty_set_erase_char(tio.c_cc[2]);
    return 0;
  }
  return -(int64_t)EINVAL;
}

static int64_t zero_user(uint64_t addr, uint64_t len) {
  uint8_t zero = 0;
  for (uint64_t i = 0; i < len; ++i) {
    if (!user_writable(addr + i, 1) || !vmm_copy_to_user(active_as(), addr + i, &zero, 1)) { return -(int64_t)EFAULT; }
  }
  return 0;
}

static void system_poweroff(void) {
  __asm__ volatile("mov x0, #0x0008\n"
                   "movk x0, #0x8400, lsl #16\n"
                   "hvc #0\n"
                   :
                   :
                   : "x0", "memory");
  for (;;) {
    __asm__ volatile("wfe");
  }
}

static void system_reboot(void) {
  __asm__ volatile("mov x0, #0x0009\n"
                   "movk x0, #0x8400, lsl #16\n"
                   "hvc #0\n"
                   :
                   :
                   : "x0", "memory");
  for (;;) {
    __asm__ volatile("wfe");
  }
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-label-as-value"
static int64_t dispatch(struct trap_frame *f) {
  static const void *linux_dispatch[] = {
    [SYS_GETCWD] = &&l_getcwd,
    [SYS_EVENTFD2] = &&l_eventfd2,
    [SYS_EPOLL_CREATE1] = &&l_epoll_create1,
    [SYS_EPOLL_CTL] = &&l_epoll_ctl,
    [SYS_EPOLL_PWAIT] = &&l_epoll_pwait,
    [SYS_DUP] = &&l_dup,
    [SYS_DUP3] = &&l_dup3,
    [SYS_FCNTL] = &&l_fcntl,
    [SYS_IOCTL] = &&l_ioctl,
    [SYS_MKDIRAT] = &&l_mkdirat,
    [SYS_UNLINKAT] = &&l_unlinkat,
    [SYS_SYMLINKAT] = &&l_symlinkat,
    [SYS_LINKAT] = &&l_linkat,
    [SYS_RENAMEAT] = &&l_renameat,
    [SYS_STATFS] = &&l_statfs,
    [SYS_FSTATFS] = &&l_fstatfs,
    [SYS_FACCESSAT] = &&l_faccessat,
    [SYS_FTRUNCATE] = &&l_ftruncate,
    [SYS_CHDIR] = &&l_chdir,
    [SYS_FCHDIR] = &&l_fchdir,
    [SYS_CHROOT] = &&l_chroot,
    [SYS_FCHMOD] = &&l_fchmod,
    [SYS_FCHMODAT] = &&l_fchmodat,
    [SYS_FCHOWNAT] = &&l_fchownat,
    [SYS_FCHOWN] = &&l_fchown,
    [SYS_OPENAT] = &&l_openat,
    [SYS_CLOSE] = &&l_close,
    [SYS_MKNODAT] = &&l_mknodat,
    [SYS_PIPE2] = &&l_pipe2,
    [SYS_GETDENTS64] = &&l_getdents64,
    [SYS_LSEEK] = &&l_lseek,
    [SYS_READ] = &&l_read,
    [SYS_WRITE] = &&l_write,
    [SYS_READV] = &&l_readv,
    [SYS_WRITEV] = &&l_writev,
    [SYS_PSELECT6] = &&l_pselect6,
    [SYS_PPOLL] = &&l_ppoll,
    [SYS_READLINKAT] = &&l_readlinkat,
    [SYS_NEWFSTATAT] = &&l_newfstatat,
    [SYS_FSTAT] = &&l_fstat,
    [SYS_FSYNC] = &&l_fsync,
    [SYS_EXIT] = &&l_exit,
    [SYS_EXIT_GROUP] = &&l_exit_group,
    [SYS_SET_TID_ADDRESS] = &&l_set_tid_address,
    [SYS_FUTEX] = &&l_futex,
    [SYS_SET_ROBUST_LIST] = &&l_set_robust_list,
    [SYS_NANOSLEEP] = &&l_nanosleep,
    [SYS_CLOCK_GETTIME] = &&l_clock_gettime,
    [SYS_CLOCK_GETRES] = &&l_clock_getres,
    [SYS_CLOCK_NANOSLEEP] = &&l_clock_nanosleep,
    [SYS_SCHED_GETAFFINITY] = &&l_sched_getaffinity,
    [SYS_SCHED_YIELD] = &&l_sched_yield,
    [SYS_KILL] = &&l_kill,
    [SYS_TKILL] = &&l_tkill,
    [SYS_TGKILL] = &&l_tgkill,
    [SYS_SIGALTSTACK] = &&l_sigaltstack,
    [SYS_RT_SIGACTION] = &&l_rt_sigaction,
    [SYS_RT_SIGPROCMASK] = &&l_zero,
    [SYS_RT_SIGRETURN] = &&l_rt_sigreturn,
    [SYS_SETGID] = &&l_setgid,
    [SYS_SETUID] = &&l_setuid,
    [SYS_SET_PGID] = &&l_setpgid,
    [SYS_GET_PGID] = &&l_getpgid,
    [SYS_GET_SID] = &&l_getsid,
    [SYS_SETSID] = &&l_setsid,
    [SYS_UNAME] = &&l_uname,
    [SYS_UMASK] = &&l_umask,
    [SYS_GETPID] = &&l_getpid,
    [SYS_GETPPID] = &&l_getppid,
    [SYS_GETUID] = &&l_getuid,
    [SYS_GETEUID] = &&l_geteuid,
    [SYS_GETGID] = &&l_getgid,
    [SYS_GETEGID] = &&l_getegid,
    [SYS_GETTID] = &&l_gettid,
    [SYS_SYSINFO] = &&l_sysinfo,
    [SYS_SOCKET] = &&l_socket,
    [SYS_BIND] = &&l_bind,
    [SYS_LISTEN] = &&l_listen,
    [SYS_ACCEPT] = &&l_accept,
    [SYS_CONNECT] = &&l_connect,
    [SYS_GETSOCKNAME] = &&l_getsockname,
    [SYS_SENDTO] = &&l_sendto,
    [SYS_RECVFROM] = &&l_recvfrom,
    [SYS_SETSOCKOPT] = &&l_zero,
    [SYS_GETSOCKOPT] = &&l_getsockopt,
    [SYS_BRK] = &&l_brk,
    [SYS_MUNMAP] = &&l_munmap,
    [SYS_MREMAP] = &&l_mremap,
    [SYS_CLONE] = &&l_clone,
    [SYS_EXECVE] = &&l_execve,
    [SYS_MMAP] = &&l_mmap,
    [SYS_MPROTECT] = &&l_mprotect,
    [SYS_MADVISE] = &&l_madvise,
    [SYS_WAIT4] = &&l_wait4,
    [SYS_PRLIMIT64] = &&l_prlimit64,
    [SYS_RENAMEAT2] = &&l_renameat2,
    [SYS_GETRANDOM] = &&l_getrandom,
    [SYS_STATX] = &&l_statx,
    [SYS_CLONE3] = &&l_enosys,
    [SYS_FACCESSAT2] = &&l_faccessat2,
  };
  static const void *spore_dispatch[] = {
    [SYS_SPORE_SNAPSHOT - SYS_SPORE_SNAPSHOT] = &&l_spore_snapshot,
    [SYS_SPORE_SPAWN - SYS_SPORE_SNAPSHOT] = &&l_spore_spawn,
    [SYS_SPORE_REAP - SYS_SPORE_SNAPSHOT] = &&l_spore_reap,
    [SYS_SPORE_RESIDENT - SYS_SPORE_SNAPSHOT] = &&l_spore_resident,
    [SYS_SPORE_SET_BUDGET - SYS_SPORE_SNAPSHOT] = &&l_spore_set_budget,
    [SYS_SPORE_APPLY_POLICY - SYS_SPORE_SNAPSHOT] = &&l_spore_apply_policy,
    [SYS_SPORE_SHUTDOWN - SYS_SPORE_SNAPSHOT] = &&l_spore_shutdown,
    [SYS_SPORE_PROCINFO - SYS_SPORE_SNAPSHOT] = &&l_spore_procinfo,
    [SYS_SPORE_FSINFO - SYS_SPORE_SNAPSHOT] = &&l_spore_fsinfo,
    [SYS_SPORE_MOUNTINFO - SYS_SPORE_SNAPSHOT] = &&l_spore_mountinfo,
  };
  uint64_t nr = f->x[8];
  uint64_t a0 = f->x[0];
  uint64_t a1 = f->x[1];
  uint64_t a2 = f->x[2];
  uint64_t a3 = f->x[3];
  uint64_t a4 = f->x[4];
  uint64_t a5 = f->x[5];

  if (!cell_syscall_allowed(nr)) {
    kprintf("[spore] syscall denied nr=%d\n", (int)nr);
    return -(int64_t)EPERM;
  }

  if (nr < sizeof(linux_dispatch) / sizeof(linux_dispatch[0]) && linux_dispatch[nr] != NULL) {
    goto *linux_dispatch[nr];
  }
  if (nr >= SYS_SPORE_SNAPSHOT) {
    uint64_t spore_index = nr - SYS_SPORE_SNAPSHOT;
    if (spore_index < sizeof(spore_dispatch) / sizeof(spore_dispatch[0]) && spore_dispatch[spore_index] != NULL) {
      goto *spore_dispatch[spore_index];
    }
  }
  goto l_unknown;

l_sched_yield:
  cell_schedule(f);
  return SYSCALL_SWITCHED;
l_read:
  return sys_read(f, a0, a1, a2);
l_write:
  return sys_write(f, a0, a1, a2);
l_readv:
  return sys_readv(f, a0, a1, a2);
l_writev:
  return sys_writev(f, a0, a1, a2);
l_pselect6:
  return sys_pselect6(f, a0, a1, a2, a3, a4);
l_ppoll:
  return sys_ppoll(f, a0, a1, a2, a3, a4);
l_eventfd2:
  return cell_fd_eventfd(a0, (int)a1);
l_epoll_create1:
  return cell_fd_epoll_create((int)a0);
l_epoll_ctl:
  return sys_epoll_ctl(a0, a1, a2, a3);
l_epoll_pwait:
  return sys_epoll_pwait(f, a0, a1, a2, a3, a4, a5);
l_nanosleep:
  return sys_nanosleep(f, a0, a1);
l_clock_nanosleep:
  return sys_clock_nanosleep(f, a0, a1, a2, a3);
l_exit:
  kprintf("[kernel] exit(%d)\n", (int)a0);
  cell_exit_thread_current((int)a0, f);
  return SYSCALL_SWITCHED;
l_exit_group:
  kprintf("[kernel] exit_group(%d)\n", (int)a0);
  cell_exit_group_current((int)a0, f);
  return SYSCALL_SWITCHED;
l_getpid:
  return cell_current_pid();
l_getppid:
  return cell_current_ppid();
l_gettid:
  return cell_current_tid();
l_setpgid:
  return cell_setpgid((int)a0, (int)a1);
l_getpgid:
  return cell_getpgid((int)a0);
l_getsid:
  return cell_getsid((int)a0);
l_setsid:
  return cell_setsid_current();
l_getuid:
  return cell_current_uid();
l_geteuid:
  return cell_current_euid();
l_getgid:
  return cell_current_gid();
l_getegid:
  return cell_current_egid();
l_setuid:
  return cell_setuid_current((uint32_t)a0) == 0 ? 0 : -(int64_t)EPERM;
l_setgid:
  return cell_setgid_current((uint32_t)a0) == 0 ? 0 : -(int64_t)EPERM;
l_zero:
  return 0;
l_clone:
  return sys_clone(f, a0, a1, a2, a3, a4);
l_futex: {
  int64_t rc = sys_futex(f, a0, a1, a2, a3);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
}
l_execve:
  return sys_execve(f, a0, a1, a2);
l_wait4: {
  int rc = cell_wait4_options((int)a0, a1, (int)a2, f);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
}
l_kill:
  if ((int)a0 == cell_current_pid() &&
      ((int)a1 == 2 || (int)a1 == 6 || (int)a1 == 9 || (int)a1 == 11 || (int)a1 == 15)) {
    cell_signal_current((int)a1, f);
    return SYSCALL_SWITCHED;
  }
  return cell_kill((int)a0, (int)a1);
l_tkill:
  if ((int)a0 == cell_current_tid() &&
      ((int)a1 == 2 || (int)a1 == 6 || (int)a1 == 9 || (int)a1 == 11 || (int)a1 == 15)) {
    cell_signal_current((int)a1, f);
    return SYSCALL_SWITCHED;
  }
  return cell_tkill((int)a0, (int)a1);
l_tgkill:
  if ((int)a1 == cell_current_pid() &&
      ((int)a2 == 2 || (int)a2 == 6 || (int)a2 == 9 || (int)a2 == 11 || (int)a2 == 15)) {
    cell_signal_current((int)a2, f);
    return SYSCALL_SWITCHED;
  }
  return cell_kill((int)a1, (int)a2);
l_rt_sigaction:
  return cell_rt_sigaction((int)a0, a1, a2, a3);
l_rt_sigreturn:
  return cell_rt_sigreturn(f) == 0 ? SYSCALL_SWITCHED : -(int64_t)EFAULT;
l_sigaltstack:
  return sys_sigaltstack(a0, a1);
l_spore_snapshot:
  return snapshot_create_current();
l_spore_spawn:
  return snapshot_spawn((int)a0, a1, a2, f);
l_spore_reap: {
  int rc = snapshot_reap((int)a0, a1, f);
  return rc == CELL_SWITCHED ? SYSCALL_SWITCHED : rc;
}
l_spore_resident:
  return (int64_t)cell_resident_pages(a0, a0 + a1);
l_spore_set_budget:
  return cell_set_budget((int)a0, a1);
l_spore_apply_policy: {
  char manifest[64];
  if (!copy_string_from_user(a0, manifest, sizeof(manifest))) { return -(int64_t)EFAULT; }
  int rc = cell_apply_policy(manifest);
  return rc == 0 ? 0 : -(int64_t)EPERM;
}
l_spore_shutdown:
  if (cell_current_euid() != 0) {
    kprintf("[kernel] shutdown denied uid=%d euid=%d\n", (int)cell_current_uid(), (int)cell_current_euid());
    return -(int64_t)EPERM;
  }
  if (a0 == 1) {
    kprintf("[kernel] reboot\n");
    system_reboot();
  } else {
    kprintf("[kernel] shutdown\n");
    system_poweroff();
  }
  return 0;
l_spore_procinfo: {
  size_t max = a1 / sizeof(struct proc_info);
  if (a0 == 0 || max == 0) { return (int64_t)cell_proc_info(NULL, 0); }
  struct proc_info infos[MAX_THREADS];
  size_t count = cell_proc_info(infos, max);
  size_t copy = count < max ? count : max;
  return user_writable(a0, copy * sizeof(infos[0])) && vmm_copy_to_user(active_as(), a0, infos, copy * sizeof(infos[0]))
           ? (int64_t)count
           : -(int64_t)EFAULT;
}
l_spore_fsinfo: {
  struct vfs_fs_info raw;
  if (!vfs_fs_info(&raw)) { return -(int64_t)EINVAL; }
  struct fs_info64 info = {
    .block_size = raw.block_size,
    .block_count = raw.block_count,
    .free_blocks = raw.free_blocks,
    .inode_count = raw.inode_count,
    .free_inodes = raw.free_inodes,
  };
  return user_writable(a0, sizeof(info)) && vmm_copy_to_user(active_as(), a0, &info, sizeof(info)) ? 0
                                                                                                   : -(int64_t)EFAULT;
}
l_spore_mountinfo: {
  size_t max = a1 / sizeof(struct mount_info64);
  if (a0 == 0 || max == 0) { return (int64_t)vfs_mount_info(NULL, 0); }
  struct vfs_mount_info raw[8];
  size_t count = vfs_mount_info(raw, max < 8 ? max : 8);
  size_t copy = count < max ? count : max;
  struct mount_info64 out[8];
  for (size_t i = 0; i < copy; ++i) {
    kmemset(&out[i], 0, sizeof(out[i]));
    kmemcpy(out[i].source, raw[i].source, sizeof(out[i].source));
    kmemcpy(out[i].target, raw[i].target, sizeof(out[i].target));
    kmemcpy(out[i].fstype, raw[i].fstype, sizeof(out[i].fstype));
    out[i].block_size = raw[i].block_size;
    out[i].block_count = raw[i].block_count;
    out[i].free_blocks = raw[i].free_blocks;
  }
  return user_writable(a0, copy * sizeof(out[0])) && vmm_copy_to_user(active_as(), a0, out, copy * sizeof(out[0]))
           ? (int64_t)count
           : -(int64_t)EFAULT;
}
l_brk:
  return sys_brk(a0);
l_mmap:
  return sys_mmap(a0, a1, a2, a3, a4, a5);
l_munmap:
  return sys_munmap(a0, a1);
l_mprotect:
  return sys_mprotect(a0, a1, a2);
l_madvise:
  return sys_madvise(a0, a1, a2);
l_mremap:
  return sys_mremap(a0, a1, a2, a3);
l_socket:
  return sys_socket(a0, a1, a2);
l_bind:
  return sys_bind(a0, a1, a2);
l_listen:
  return sys_listen(a0, a1);
l_accept:
  return sys_accept(f, a0, a1, a2);
l_connect:
  return sys_connect(a0, a1, a2);
l_sendto:
  return sys_sendto(f, a0, a1, a2, a3, a4, a5);
l_recvfrom:
  return sys_recvfrom(f, a0, a1, a2, a3, a4, a5);
l_getsockname:
  return sys_getsockname(a0, a1, a2);
l_getsockopt:
  return sys_getsockopt(a0, a1, a2, a3, a4);
l_openat:
  return sys_openat(a0, a1, a2);
l_close:
  return cell_fd_close((int)a0);
l_pipe2:
  return cell_fd_pipe2(a0, (int)a1);
l_lseek:
  return cell_fd_lseek((int)a0, (int64_t)a1, (int)a2);
l_fstat:
  return sys_fstat(a0, a1);
l_newfstatat:
  return sys_newfstatat(a0, a1, a2, a3);
l_faccessat:
  return sys_faccessat(a0, a1, a2, 0);
l_faccessat2:
  return sys_faccessat(a0, a1, a2, a3);
l_getdents64:
  return sys_getdents64(a0, a1, a2);
l_readlinkat:
  return sys_readlinkat(a0, a1, a2, a3);
l_dup:
  return cell_fd_dup((int)a0, 0);
l_dup3:
  return cell_fd_dup3((int)a0, (int)a1, (int)a2);
l_fcntl:
  if (a1 == F_DUPFD) { return cell_fd_dup((int)a0, (int)a2); }
  if (a1 == F_GETFD || a1 == F_SETFD || a1 == F_GETFL || a1 == F_SETFL) { return 0; }
  return -(int64_t)EINVAL;
l_getcwd:
  return sys_getcwd(a0, a1);
l_prlimit64:
  return sys_prlimit64(a0, a1, a2, a3);
l_sysinfo:
  return zero_user(a0 == 0 ? a2 : a0, 128);
l_uname:
  return sys_uname(a0);
l_umask:
  return 022;
l_chroot:
  return sys_chroot(a0);
l_set_robust_list:
  return cell_set_robust_list_current(a0);
l_sched_getaffinity:
  return sys_sched_getaffinity(a2, a1);
l_set_tid_address:
  return cell_set_tid_address_current(a0);
l_ioctl:
  return sys_ioctl(a0, a1, a2);
l_mknodat:
  return sys_mknodat(a0, a1, a2);
l_mkdirat:
  return sys_mkdirat(a0, a1);
l_linkat:
  return sys_linkat(a0, a1, a2, a3, a4);
l_symlinkat:
  return sys_symlinkat(a0, a1, a2);
l_unlinkat:
  return sys_unlinkat(a0, a1);
l_renameat:
  return sys_renameat(a0, a1, a2, a3);
l_renameat2:
  return a4 == 0 ? sys_renameat(a0, a1, a2, a3) : -(int64_t)EINVAL;
l_statfs:
  return sys_statfs(a0, a1);
l_fstatfs:
  return sys_fstatfs(a0, a1);
l_statx:
  return sys_statx(a0, a1, a2, a3, a4);
l_fchmod:
  return sys_fchmod(a0, a1);
l_fchmodat:
  return sys_fchmodat(a0, a1, a2, a3);
l_fchownat:
  return sys_fchownat(a0, a1, a2, a3, a4);
l_fchown:
  return sys_fchown(a0, a1, a2);
l_ftruncate:
  return sys_ftruncate(a0, a1);
l_chdir:
  return sys_chdir(a0);
l_fchdir:
  return cell_fd_is_dir((int)a0) ? 0 : -(int64_t)ENOTDIR;
l_fsync:
  return 0;
l_getrandom:
  return sys_getrandom(a0, a1);
l_clock_gettime:
  return sys_clock_gettime(a0, a1);
l_clock_getres:
  return sys_clock_getres(a0, a1);
l_enosys:
  return -(int64_t)ENOSYS;
l_unknown:
  kprintf("[kernel] syscall %d ENOSYS\n", (int)nr);
  return -(int64_t)ENOSYS;
}
#pragma clang diagnostic pop

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
    kprintf("[kernel] lower sync fault ec=%x dfsc=%x write=%u esr=%x elr=%p far=%p\n", (unsigned)ec,
            (unsigned)dfsc, write ? 1u : 0u, (unsigned)frame->esr_el1, (void *)(uintptr_t)frame->elr_el1,
            (void *)(uintptr_t)far);
    cell_dump_current_fault(frame->esr_el1, frame->elr_el1, far);
    cell_signal_current(11, frame);
    return;
  }
  int64_t ret = dispatch(frame);
  if (ret != SYSCALL_SWITCHED) { frame->x[0] = (uint64_t)ret; }
}
