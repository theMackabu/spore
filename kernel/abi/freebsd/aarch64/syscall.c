#include "abi/freebsd/aarch64.h"
#include "arch/aarch64/regs.h"
#include "abi/linux/aarch64/syscall.h"
#include "abi/linux/aarch64/numbers.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "mm/pmm.h"
#include "proc/thread.h"

static unsigned freebsd_errno(unsigned error) {
  switch (error) {
  case 11: return 35;
  case 35: return 11;
  case 36: return 63;
  case 40: return 62;
  case 75: return 84;
  case 95: return 45;
  case 38: return 78;
  case 88: return 38;
  case 104: return 54;
  case 107: return 57;
  case 110: return 60;
  default: return error <= 34 ? error : 5;
  }
}

void freebsd_aarch64_result(struct trap_frame *frame, int64_t result) {
  constexpr uint64_t carry = 1ull << 29;
  if (result < 0) {
    frame->x[0] = freebsd_errno((unsigned)-result);
    frame->spsr_el1 |= carry;
  } else {
    frame->x[0] = result;
    frame->x[1] = 0;
    frame->spsr_el1 &= ~carry;
  }
}

static int64_t freebsd_mmap(struct trap_frame *frame, const uint64_t *args) {
  enum { SHARED = 1, PRIVATE = 2, FIXED = 16, ANON = 0x1000, EXCL = 0x4000 };
  uint32_t flags = args[3];
  unsigned shift = flags >> 24;
  flags &= 0xffffff;
  if ((args[2] & ~7ull) || flags & ~(SHARED | PRIVATE | FIXED | ANON | EXCL) || ((flags & EXCL) && !(flags & FIXED)) ||
      (shift && (shift < 12 || shift >= 48))) { return -22; }
  uint64_t addr = args[0], len = args[1];
  if (shift) {
    uint64_t alignment = 1ull << shift;
    if (!len || len > USER_VA_LIMIT) { return -22; }
    if (flags & FIXED) {
      if (addr & (alignment - 1)) { return -22; }
    } else {
      const struct vma_list *vmas = cell_domain_vmas_const(cell_current_domain_internal());
      uint64_t hint = addr ? addr : cell_current_as()->mmap_base;
      uint64_t size = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
      if (!vma_find_aligned_gap(vmas, hint, USER_VA_LIMIT, size, alignment, &addr) &&
          !vma_find_aligned_gap(vmas, USER_MMAP_BASE, USER_VA_LIMIT, size, alignment, &addr)) { return -12; }
    }
  }
  uint64_t translated = (flags & (PRIVATE | SHARED)) | ((flags & ANON) ? 0x20 : 0) |
                        ((flags & EXCL) ? 0x100000 : (flags & FIXED));
  const uint64_t linux_args[] = {addr, len, args[2], translated, args[4], args[5]};
  return linux_aarch64_dispatch(frame, SYS_MMAP, linux_args);
}

void freebsd_aarch64_syscall(struct trap_frame *frame) {
  uint64_t nr = trap_syscall_number(frame);
  uint64_t *args = frame->x;
  if (nr == 0 || nr == 198) { nr = *args++; }
  uint64_t syscall;
  int64_t result;
  switch (nr) {
  case 1: syscall = SYS_EXIT_GROUP; break;
  case 3: syscall = SYS_READ; break;
  case 4: syscall = SYS_WRITE; break;
  case 20: syscall = SYS_GETPID; break;
  case 24: syscall = SYS_GETUID; break;
  case 25: syscall = SYS_GETEUID; break;
  case 39: syscall = SYS_GETPPID; break;
  case 43: syscall = SYS_GETEGID; break;
  case 47: syscall = SYS_GETGID; break;
  case 58: {
    const uint64_t at[] = {(uint64_t)-100, args[0], args[1], args[2], 0, 0};
    result = linux_aarch64_dispatch(frame, SYS_READLINKAT, at);
    goto done;
  }
  case 73: syscall = SYS_MUNMAP; break;
  case 74:
    if (args[2] & ~7ull) { result = -22; goto done; }
    syscall = SYS_MPROTECT; break;
  case 120: syscall = SYS_READV; break;
  case 121: syscall = SYS_WRITEV; break;
  case 477: result = freebsd_mmap(frame, args); goto done;
  case 563: syscall = SYS_GETRANDOM; break;
  default: result = -38; goto done;
  }
  result = linux_aarch64_dispatch(frame, syscall, args);
done:
  if (result != CELL_SWITCHED) {
    freebsd_aarch64_result(frame, result);
    (void)cell_deliver_pending_signals_current(frame);
  }
}
