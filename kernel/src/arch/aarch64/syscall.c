#include "arch/aarch64/exceptions.h"

#include "arch/aarch64/regs.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "pl011.h"

#include <stddef.h>
#include <stdint.h>

enum {
    EC_SVC_A64 = 0x15,
    SYS_IOCTL = 29,
    SYS_WRITE = 64,
    SYS_READV = 65,
    SYS_WRITEV = 66,
    SYS_CLOCK_GETTIME = 113,
    SYS_EXIT_GROUP = 94,
    SYS_SET_TID_ADDRESS = 96,
    SYS_RT_SIGPROCMASK = 135,
    SYS_BRK = 214,
    SYS_MUNMAP = 215,
    SYS_MMAP = 222,
    SYS_MPROTECT = 226,
    SYS_GETRANDOM = 278,
    MAP_PRIVATE = 0x02,
    MAP_ANONYMOUS = 0x20,
    PROT_EXEC = 0x4,
    PROT_WRITE = 0x2,
    PROT_READ = 0x1,
    EFAULT = 14,
    EINVAL = 22,
    ENOSYS = 38,
    MAX_IOVCNT = 1024,
};

struct iovec64 {
    uint64_t base;
    uint64_t len;
};

struct timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static struct user_address_space *current_as;
static uint64_t rng_state = 0x73706f72652d7630ull;

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

static void poweroff(void) {
    __asm__ volatile(
        "mov x0, #0x0008\n"
        "movk x0, #0x8400, lsl #16\n"
        "hvc #0\n"
        :
        :
        : "x0", "memory");
    for (;;) {
        __asm__ volatile("wfe");
    }
}

void syscall_set_address_space(struct user_address_space *as) {
    current_as = as;
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1 && fd != 2) {
        return -(int64_t)EINVAL;
    }
    if (!vmm_user_range_accessible(current_as, buf, len, VMM_ACCESS_READ)) {
        return -(int64_t)EFAULT;
    }
    for (uint64_t i = 0; i < len; ++i) {
        char c;
        if (!vmm_copy_from_user(current_as, &c, buf + i, 1)) {
            return -(int64_t)EFAULT;
        }
        pl011_putc(c);
    }
    return (int64_t)len;
}

static int64_t sys_writev(uint64_t fd, uint64_t iov, uint64_t iovcnt) {
    if (iovcnt > MAX_IOVCNT) {
        return -(int64_t)EINVAL;
    }
    uint64_t iov_bytes;
    if (!checked_add(0, iovcnt * sizeof(struct iovec64), &iov_bytes) ||
        (iovcnt != 0 && iov_bytes / sizeof(struct iovec64) != iovcnt) ||
        !vmm_user_range_accessible(current_as, iov, iov_bytes, VMM_ACCESS_READ)) {
        return -(int64_t)EFAULT;
    }

    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; ++i) {
        struct iovec64 v;
        if (!vmm_copy_from_user(current_as, &v, iov + i * sizeof(v), sizeof(v))) {
            return -(int64_t)EFAULT;
        }
        int64_t wrote = sys_write(fd, v.base, v.len);
        if (wrote < 0) {
            return wrote;
        }
        total += wrote;
    }
    return total;
}

static int64_t sys_brk(uint64_t requested) {
    if (requested == 0) {
        return (int64_t)current_as->brk_current;
    }
    if (requested < current_as->brk_base) {
        return (int64_t)current_as->brk_current;
    }
    uint64_t old_end = align_up(current_as->brk_current, PAGE_SIZE);
    uint64_t new_end = align_up(requested, PAGE_SIZE);
    if (new_end < requested) {
        return (int64_t)current_as->brk_current;
    }
    for (uint64_t va = old_end; va < new_end; va += PAGE_SIZE) {
        if (!vmm_alloc_page(current_as, va, VMM_USER_READ | VMM_USER_WRITE)) {
            return (int64_t)current_as->brk_current;
        }
    }
    current_as->brk_current = requested;
    return (int64_t)requested;
}

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags) {
    if (len == 0 || (flags & (MAP_PRIVATE | MAP_ANONYMOUS)) != (MAP_PRIVATE | MAP_ANONYMOUS)) {
        return -(int64_t)EINVAL;
    }
    uint64_t base = addr != 0 ? align_down(addr, PAGE_SIZE) : current_as->mmap_base;
    uint64_t raw_end;
    if (!checked_add(base, len, &raw_end)) {
        return -(int64_t)EINVAL;
    }
    uint64_t end = align_up(raw_end, PAGE_SIZE);
    if (end < raw_end) {
        return -(int64_t)EINVAL;
    }
    uint32_t vflags = VMM_USER_READ;
    if ((prot & PROT_WRITE) != 0) {
        vflags |= VMM_USER_WRITE;
    }
    if ((prot & PROT_EXEC) != 0) {
        vflags |= VMM_USER_EXEC;
    }
    for (uint64_t va = base; va < end; va += PAGE_SIZE) {
        if (!vmm_alloc_page(current_as, va, vflags)) {
            return -(int64_t)EINVAL;
        }
    }
    if (addr == 0) {
        current_as->mmap_base = end;
    }
    return (int64_t)base;
}

static int64_t sys_getrandom(uint64_t buf, uint64_t len) {
    if (!vmm_user_range_accessible(current_as, buf, len, VMM_ACCESS_WRITE)) {
        return -(int64_t)EFAULT;
    }
    for (uint64_t i = 0; i < len; ++i) {
        rng_state = rng_state * 6364136223846793005ull + 1;
        uint8_t byte = (uint8_t)(rng_state >> 32);
        if (!vmm_copy_to_user(current_as, buf + i, &byte, 1)) {
            return -(int64_t)EFAULT;
        }
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
    return vmm_copy_to_user(current_as, tp, &ts, sizeof(ts)) ? 0 : -(int64_t)EFAULT;
}

static int64_t dispatch(struct trap_frame *f) {
    uint64_t nr = f->x[8];
    uint64_t a0 = f->x[0];
    uint64_t a1 = f->x[1];
    uint64_t a2 = f->x[2];
    uint64_t a3 = f->x[3];
    uint64_t a4 = f->x[4];
    uint64_t a5 = f->x[5];
    (void)a4;
    (void)a5;

    switch (nr) {
    case SYS_WRITE:
        return sys_write(a0, a1, a2);
    case SYS_WRITEV:
        return sys_writev(a0, a1, a2);
    case SYS_READV:
        return 0;
    case SYS_EXIT_GROUP:
        kprintf("[kernel] exit_group(%d)\n", (int)a0);
        poweroff();
    case SYS_BRK:
        return sys_brk(a0);
    case SYS_MMAP:
        return sys_mmap(a0, a1, a2, a3);
    case SYS_MUNMAP:
    case SYS_MPROTECT:
    case SYS_RT_SIGPROCMASK:
        return 0;
    case SYS_SET_TID_ADDRESS:
        return 1;
    case SYS_IOCTL:
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
        kprintf("[kernel] lower sync fault ec=%x esr=%x elr=%p far=%p\n",
                (unsigned)ec,
                (unsigned)frame->esr_el1,
                (void *)(uintptr_t)frame->elr_el1,
                (void *)(uintptr_t)far);
        poweroff();
    }
    frame->x[0] = (uint64_t)dispatch(frame);
}
