#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SYS_EXIT_GROUP 94
#define SYS_WRITE 64
#define SYS_SPORE_SNAPSHOT 0x4000
#define SYS_SPORE_SPAWN 0x4001
#define SYS_SPORE_REAP 0x4002

static volatile uint64_t shared_word = 0x1111000000000000ull;

static long raw_syscall3(long nr, long a0, long a1, long a2) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long raw_syscall1(long nr, long a0) {
    return raw_syscall3(nr, a0, 0, 0);
}

static void raw_write(const char *s) {
    const char *p = s;
    while (*p != '\0') {
        ++p;
    }
    (void)raw_syscall3(SYS_WRITE, 1, (long)s, p - s);
}

static void raw_dec(uint64_t value) {
    char buf[32];
    int i = 0;
    if (value == 0) {
        raw_write("0");
        return;
    }
    while (value != 0) {
        buf[i++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (i > 0) {
        char c[2] = {buf[--i], 0};
        raw_write(c);
    }
}

static void raw_hex(uint64_t value) {
    char buf[17];
    int i = 0;
    raw_write("0x");
    if (value == 0) {
        raw_write("0");
        return;
    }
    while (value != 0) {
        uint64_t d = value & 0xf;
        buf[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value >>= 4;
    }
    while (i > 0) {
        char c[2] = {buf[--i], 0};
        raw_write(c);
    }
}

static void branch_main(uint64_t value) {
    shared_word = value;
    if (shared_word == value) {
        raw_syscall1(SYS_EXIT_GROUP, (long)(value & 0xff));
    }
    raw_syscall1(SYS_EXIT_GROUP, 1);
}

static void touch_resident(size_t mib) {
    size_t len = mib * 1024 * 1024;
    volatile uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        raw_write("[spore] mmap failed\n");
        exit(1);
    }
    for (size_t off = 0; off < len; off += 4096) {
        p[off] = (uint8_t)(off >> 12);
    }
}

static uint64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int stock_fork_wait_test(void) {
    volatile uint64_t local = 0xaaaa;
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0) {
        local = 0xbbbb;
        _exit(local == 0xbbbb ? 42 : 2);
    }

    local = 0xcccc;
    int status = 0;
    pid_t got = wait(&status);
    return got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42 &&
           local == 0xcccc;
}

int main(void) {
    touch_resident(100);

    int snap = (int)raw_syscall3(SYS_SPORE_SNAPSHOT, 0, 0, 0);
    raw_write("[spore] snapshot base -> snap ");
    raw_dec((uint64_t)snap);
    raw_write(" (resident 100 MB)\n");
    raw_write("[spore] spawn x4 from snap ");
    raw_dec((uint64_t)snap);
    raw_write("\n");

    const uint64_t values[4] = {
        0x111100000000000aull,
        0x222200000000000bull,
        0x333300000000000cull,
        0x444400000000000dull,
    };
    int cells[4];
    for (int i = 0; i < 4; ++i) {
        cells[i] = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)branch_main, (long)values[i]);
    }

    int distinct = 1;
    int seen[256] = {0};
    for (int i = 0; i < 4; ++i) {
        int status = 0;
        int got = (int)raw_syscall3(SYS_SPORE_REAP, cells[i], (long)&status, 0);
        int result = (status >> 8) & 0xff;
        raw_write("[cell ");
        raw_dec((uint64_t)(i + 1));
        raw_write("] result = ");
        raw_hex(values[i]);
        raw_write("\n");
        (void)got;
        if (seen[result]) {
            distinct = 0;
        }
        seen[result] = 1;
    }
    raw_write("[spore] reaped 4 cells, ");
    raw_write(distinct ? "all" : "not all");
    raw_write(" distinct\n");

    uint64_t lat[3];
    const int sizes[3] = {10, 50, 100};
    for (int i = 0; i < 3; ++i) {
        uint64_t start = monotonic_us();
        int child = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)branch_main, (long)(0x80 + i));
        lat[i] = monotonic_us() - start;
        int status = 0;
        (void)raw_syscall3(SYS_SPORE_REAP, child, (long)&status, 0);
        (void)sizes[i];
    }
    raw_write("[spore] fork latency: 10MB=");
    raw_dec(lat[0]);
    raw_write("us 50MB=");
    raw_dec(lat[1]);
    raw_write("us 100MB=");
    raw_dec(lat[2]);
    raw_write("us (flat)\n");

    int stock_ok = stock_fork_wait_test();
    raw_write("[spore] stock fork()/wait() isolation test: ");
    raw_write(stock_ok ? "PASS" : "FAIL");
    raw_write("\n");
    return 0;
}
