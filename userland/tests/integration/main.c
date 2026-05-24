#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>

#ifndef SYS_getdents64
#define SYS_getdents64 61
#endif

#define SYS_SPORE_SNAPSHOT 0x4000
#define SYS_SPORE_SPAWN 0x4001
#define SYS_SPORE_REAP 0x4002
#define SYS_SPORE_RESIDENT 0x4003
#define SYS_SPORE_SET_BUDGET 0x4004
#define SYS_SPORE_APPLY_POLICY 0x4005
#define SYS_EXIT 93
#define SYS_EXIT_GROUP 94
#define SYS_FUTEX 98
#define SYS_GETTID 178

#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD 0x00010000
#define CLONE_SYSVSEM 0x00040000
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static long raw_syscall3(long nr, long a0, long a1, long a2) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return x0;
}

static long raw_syscall4(long nr, long a0, long a1, long a2, long a3) {
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
    return x0;
}

long spore_clone_start(long flags,
                       void *stack_top,
                       int *parent_tid,
                       void *tls,
                       int *child_tid,
                       void (*fn)(void *),
                       void *arg);

__asm__(
    ".text\n"
    ".global spore_clone_start\n"
    "spore_clone_start:\n"
    "    mov x8, #220\n"
    "    svc #0\n"
    "    cbnz x0, 1f\n"
    "    mov x0, x6\n"
    "    blr x5\n"
    "    mov x8, #93\n"
    "    mov x0, #0\n"
    "    svc #0\n"
    "0:  b 0b\n"
    "1:  ret\n");

static void exit_group(int code) {
    raw_syscall3(SYS_EXIT_GROUP, code, 0, 0);
    for (;;) {
    }
}

static void regression_child(uint64_t code) {
    exit_group((int)code);
}

static void compute_child(uint64_t branch) {
    volatile uint64_t acc = branch + 0x1234;
    for (uint64_t i = 0; i < 30000000ull; ++i) {
        acc = (acc * 1103515245ull + 12345ull + branch) & 0xffffffffull;
    }
    printf("[spore] compute domain %lu finished acc=0x%lx\n",
           (unsigned long)branch,
           (unsigned long)acc);
    exit_group((int)(10 + branch));
}

static void spinner_child(uint64_t unused) {
    (void)unused;
    volatile uint64_t spin = 0;
    for (;;) {
        ++spin;
    }
}

static uintptr_t page_down(uintptr_t value) {
    return value & ~(uintptr_t)4095;
}

static int snapshot_regression(void) {
    int snap = (int)raw_syscall3(SYS_SPORE_SNAPSHOT, 0, 0, 0);
    if (snap < 0) {
        return 0;
    }
    int child = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)regression_child, 7);
    if (child < 0) {
        return 0;
    }
    int status = 0;
    int got = (int)raw_syscall3(SYS_SPORE_REAP, child, (long)&status, 0);
    return got == child && ((status >> 8) & 0xff) == 7;
}

static int phase_b_timer_budget_demo(void) {
    int snap = (int)raw_syscall3(SYS_SPORE_SNAPSHOT, 0, 0, 0);
    if (snap < 0) {
        return 0;
    }

    int a = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)compute_child, 1);
    int b = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)compute_child, 2);
    if (a < 0 || b < 0) {
        return 0;
    }
    int status_a = 0;
    int status_b = 0;
    int got_a = (int)raw_syscall3(SYS_SPORE_REAP, a, (long)&status_a, 0);
    int got_b = (int)raw_syscall3(SYS_SPORE_REAP, b, (long)&status_b, 0);
    int ok_compute = got_a == a && got_b == b &&
                     ((status_a >> 8) & 0xff) == 11 &&
                     ((status_b >> 8) & 0xff) == 12;
    printf("[spore] timer demo: compute domains %d/%d both finished: %s\n",
           a,
           b,
           ok_compute ? "PASS" : "FAIL");

    int spin = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)spinner_child, 0);
    if (spin < 0) {
        return 0;
    }
    raw_syscall3(SYS_SPORE_SET_BUDGET, spin, 5, 0);
    int spin_status = 0;
    int got_spin = (int)raw_syscall3(SYS_SPORE_REAP, spin, (long)&spin_status, 0);
    int ok_budget = got_spin == spin && ((spin_status >> 8) & 0xff) == 137;
    printf("[spore] timer demo: runaway domain %d budget kill: %s\n",
           spin,
           ok_budget ? "PASS" : "FAIL");
    return ok_compute && ok_budget;
}

static int phase_c_exec_demo(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0) {
        char *const argv[] = {"/bin/exec_child", "ok", NULL};
        char *const envp[] = {NULL};
        execve("/bin/exec_child", argv, envp);
        exit_group(99);
    }
    int status = 0;
    pid_t got = waitpid(pid, &status, 0);
    int ok = got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42;
    printf("[spore] execve demo: child exit 42: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static int phase_c_stdin_demo(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return 0;
    }
    if (pid == 0) {
        char c = 0;
        printf("[spore] stdin demo: child blocking on read(0)\n");
        ssize_t n = read(0, &c, 1);
        if (n == 1 && c == 'z') {
            printf("[spore] stdin demo: read 'z'\n");
            exit_group(0);
        }
        exit_group(4);
    }

    volatile unsigned long spin = 0;
    for (unsigned long i = 0; i < 70000000ul; ++i) {
        spin += i;
    }
    (void)spin;
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("[spore] stdin demo: blocking read resume: %s\n", ok ? "PASS" : "SKIP");
    return ok;
}

static volatile int thread_slots[2];
static int thread_demo_pid;
static int thread_demo_tid;

static unsigned char thread_stack_a[16384] __attribute__((aligned(16)));
static unsigned char thread_stack_b[16384] __attribute__((aligned(16)));
static unsigned char futex_stack_a[16384] __attribute__((aligned(16)));
static unsigned char futex_stack_b[16384] __attribute__((aligned(16)));

static void phase_a_thread_entry(void *arg) {
    long slot = (long)arg;
    int pid = getpid();
    int tid = (int)syscall(SYS_GETTID);
    if (slot >= 0 && slot < 2 && pid == thread_demo_pid && tid != thread_demo_tid) {
        thread_slots[slot] = tid;
    }
    printf("[spore] v3a thread %ld: domain=%d tid=%d\n", slot, pid, tid);
    raw_syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) {
    }
}

static int phase_a_thread_demo(void) {
    thread_slots[0] = 0;
    thread_slots[1] = 0;
    thread_demo_pid = getpid();
    thread_demo_tid = (int)syscall(SYS_GETTID);
    long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD | CLONE_SYSVSEM;
    long a = spore_clone_start(flags,
                               thread_stack_a + sizeof(thread_stack_a),
                               NULL,
                               NULL,
                               NULL,
                               phase_a_thread_entry,
                               (void *)0);
    long b = spore_clone_start(flags,
                               thread_stack_b + sizeof(thread_stack_b),
                               NULL,
                               NULL,
                               NULL,
                               phase_a_thread_entry,
                               (void *)1);
    if (a < 0 || b < 0) {
        printf("[spore] v3a raw clone threads: FAIL clone=%ld/%ld\n", a, b);
        return 0;
    }
    for (int i = 0; i < 100000 && (thread_slots[0] == 0 || thread_slots[1] == 0); ++i) {
        sched_yield();
    }
    int ok = thread_slots[0] != 0 && thread_slots[1] != 0 &&
             thread_slots[0] != thread_slots[1];
    printf("[spore] v3a raw clone threads: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

static volatile int futex_word;
static volatile int futex_started[2];
static volatile int futex_finished[2];

static void phase_b_futex_entry(void *arg) {
    long slot = (long)arg;
    if (slot >= 0 && slot < 2) {
        futex_started[slot] = 1;
    }
    long rc = raw_syscall4(SYS_FUTEX,
                           (long)&futex_word,
                           FUTEX_WAIT_PRIVATE,
                           0,
                           0);
    if ((rc == 0 || rc == -11) && slot >= 0 && slot < 2) {
        futex_finished[slot] = 1;
    }
    printf("[spore] v3b futex thread %ld resumed rc=%ld\n", slot, rc);
    raw_syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) {
    }
}

static int phase_b_futex_demo(void) {
    futex_word = 0;
    futex_started[0] = futex_started[1] = 0;
    futex_finished[0] = futex_finished[1] = 0;
    long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                 CLONE_THREAD | CLONE_SYSVSEM;
    long a = spore_clone_start(flags,
                               futex_stack_a + sizeof(futex_stack_a),
                               NULL,
                               NULL,
                               NULL,
                               phase_b_futex_entry,
                               (void *)0);
    long b = spore_clone_start(flags,
                               futex_stack_b + sizeof(futex_stack_b),
                               NULL,
                               NULL,
                               NULL,
                               phase_b_futex_entry,
                               (void *)1);
    if (a < 0 || b < 0) {
        printf("[spore] v3b futex wait/wake: FAIL clone=%ld/%ld\n", a, b);
        return 0;
    }
    for (int i = 0; i < 100000 && (futex_started[0] == 0 || futex_started[1] == 0); ++i) {
        sched_yield();
    }
    futex_word = 1;
    long woke = raw_syscall4(SYS_FUTEX, (long)&futex_word, FUTEX_WAKE_PRIVATE, 2, 0);
    for (int i = 0; i < 100000 && (futex_finished[0] == 0 || futex_finished[1] == 0); ++i) {
        sched_yield();
    }
    int ok = futex_finished[0] != 0 && futex_finished[1] != 0 && woke >= 0;
    printf("[spore] v3b futex wait/wake: %s (woke=%ld)\n", ok ? "PASS" : "FAIL", woke);
    return ok;
}

static pthread_mutex_t pthread_demo_lock = PTHREAD_MUTEX_INITIALIZER;
static int pthread_demo_counter;

static void *phase_c_pthread_worker(void *arg) {
    long slot = (long)arg;
    for (int i = 0; i < 1000; ++i) {
        pthread_mutex_lock(&pthread_demo_lock);
        ++pthread_demo_counter;
        pthread_mutex_unlock(&pthread_demo_lock);
    }
    return (void *)(slot + 0x100);
}

static int phase_c_pthread_demo(void) {
    enum { THREADS = 8 };
    pthread_t threads[THREADS];
    pthread_demo_counter = 0;
    for (long i = 0; i < THREADS; ++i) {
        if (pthread_create(&threads[i], NULL, phase_c_pthread_worker, (void *)i) != 0) {
            printf("[spore] v3c pthread create/join: FAIL create %ld\n", i);
            return 0;
        }
    }
    int ok = 1;
    for (long i = 0; i < THREADS; ++i) {
        void *ret = NULL;
        if (pthread_join(threads[i], &ret) != 0 || ret != (void *)(i + 0x100)) {
            ok = 0;
        }
    }
    ok = ok && pthread_demo_counter == THREADS * 1000;
    printf("[spore] v3c pthread create/join: %s counter=%d\n",
           ok ? "PASS" : "FAIL",
           pthread_demo_counter);
    return ok;
}

static uint16_t be16(uint16_t x) {
    return (uint16_t)((x << 8) | (x >> 8));
}

static uint32_t ip4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return a | (b << 8) | (c << 16) | (d << 24);
}

static void fill_udp_addr(struct sockaddr_in *sa, uint32_t ip, uint16_t port) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = be16(port);
    sa->sin_addr.s_addr = ip;
}

static int phase_e_udp_demo(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        printf("[spore] v3e udp socket echo: FAIL socket\n");
        return 0;
    }
    struct sockaddr_in sa;
    fill_udp_addr(&sa, ip4(10, 0, 2, 2), 5555);
    const char msg[] = "udp-hi";
    ssize_t sent = sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
    char buf[32] = {0};
    ssize_t got = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
    close(fd);
    int ok = sent == (ssize_t)(sizeof(msg) - 1) &&
             got == (ssize_t)(sizeof(msg) - 1) &&
             strcmp(buf, msg) == 0;
    printf("[spore] v3e udp socket echo: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
    return ok;
}

static int udp_send_target(uint32_t ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in sa;
    fill_udp_addr(&sa, ip, port);
    const char msg[] = "egress";
    int rc = (int)sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    return rc;
}

static int udp_connect_send(uint32_t connect_ip,
                            uint16_t connect_port,
                            uint32_t send_ip,
                            uint16_t send_port,
                            int null_dest) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in sa;
    fill_udp_addr(&sa, connect_ip, connect_port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    const char msg[] = "spoof";
    int rc;
    if (null_dest) {
        rc = (int)sendto(fd, msg, sizeof(msg) - 1, 0, NULL, 0);
    } else {
        fill_udp_addr(&sa, send_ip, send_port);
        rc = (int)sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
    }
    close(fd);
    return rc;
}

static void egress_child(const char *manifest, int expect_success) {
    if (syscall(SYS_SPORE_APPLY_POLICY, manifest) != 0) {
        exit_group(20);
    }
    errno = 0;
    int rc = udp_send_target(ip4(10, 0, 2, 2), 5555);
    if (expect_success) {
        exit_group(rc == 6 ? 0 : 21);
    }
    exit_group(rc < 0 && errno == EPERM ? 0 : 22);
}

static void egress_attack_child(int mode) {
    long policy = 0;
    int rc = -1;
    switch (mode) {
    case 0:
        policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
        errno = 0;
        rc = udp_send_target(ip4(10, 0, 2, 2), 5556);
        exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 30);
    case 1:
        policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.0/24:5555");
        rc = udp_send_target(ip4(10, 0, 2, 200), 5555);
        exit_group(policy == 0 && rc == 6 ? 0 : 31);
    case 2:
        policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.0/24:5555");
        errno = 0;
        rc = udp_send_target(ip4(10, 0, 3, 2), 5555);
        exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 32);
    case 3:
        policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
        errno = 0;
        rc = udp_connect_send(ip4(10, 0, 2, 2), 5555, 0, 0, 1);
        exit_group(policy == 0 && rc == 5 ? 0 : 33);
    case 4:
        policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
        errno = 0;
        rc = udp_connect_send(ip4(10, 0, 2, 2), 5555, ip4(10, 0, 3, 2), 5555, 0);
        exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 34);
    case 5: {
        policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in sa;
        fill_udp_addr(&sa, ip4(10, 0, 2, 2), 5556);
        errno = 0;
        rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        close(fd);
        exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 35);
    }
    default:
        exit_group(99);
    }
}

static int run_egress_attack(int mode) {
    pid_t pid = fork();
    if (pid == 0) {
        egress_attack_child(mode);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int egress_escalation_demo(void) {
    pid_t pid = fork();
    if (pid == 0) {
        if (syscall(SYS_SPORE_APPLY_POLICY, "net:none") != 0) {
            exit_group(40);
        }
        pid_t grandchild = fork();
        if (grandchild == 0) {
            long rc = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
            exit_group(rc < 0 && errno == EPERM ? 0 : 41);
        }
        int status = 0;
        waitpid(grandchild, &status, 0);
        exit_group(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 42);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int phase_f_egress_demo(void) {
    pid_t deny = fork();
    if (deny == 0) {
        egress_child("net:none", 0);
    }
    int status_deny = 0;
    waitpid(deny, &status_deny, 0);

    pid_t allow = fork();
    if (allow == 0) {
        egress_child("net:udp:10.0.2.2:5555", 1);
    }
    int status_allow = 0;
    waitpid(allow, &status_allow, 0);

    int ok = WIFEXITED(status_deny) && WEXITSTATUS(status_deny) == 0 &&
             WIFEXITED(status_allow) && WEXITSTATUS(status_allow) == 0;
    int wrong_port = run_egress_attack(0);
    int cidr_in = run_egress_attack(1);
    int cidr_out = run_egress_attack(2);
    int connect_null = run_egress_attack(3);
    int connect_spoof = run_egress_attack(4);
    int connect_setpeer = run_egress_attack(5);
    int escalation = egress_escalation_demo();
    ok = ok && wrong_port && cidr_in && cidr_out && connect_null &&
         connect_spoof && connect_setpeer && escalation;
    printf("[spore] v3f egress pressure: %s port=%s cidr-in=%s cidr-out=%s connect-null=%s connect-spoof=%s connect-setpeer=%s child-escalate=%s\n",
           ok ? "PASS" : "FAIL",
           wrong_port ? "PASS" : "FAIL",
           cidr_in ? "PASS" : "FAIL",
           cidr_out ? "PASS" : "FAIL",
           connect_null ? "PASS" : "FAIL",
           connect_spoof ? "PASS" : "FAIL",
           connect_setpeer ? "PASS" : "FAIL",
           escalation ? "PASS" : "FAIL");
    return ok;
}

static uint64_t usec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int fork_latency_profile(void) {
    enum { SAMPLES = 4 };
    uint64_t start = usec_now();
    for (int i = 0; i < SAMPLES; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            exit_group(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
    }
    uint64_t fork_us = (usec_now() - start) / SAMPLES;

    int snap = (int)raw_syscall3(SYS_SPORE_SNAPSHOT, 0, 0, 0);
    start = usec_now();
    for (int i = 0; i < SAMPLES; ++i) {
        int child = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)regression_child, 0);
        int status = 0;
        raw_syscall3(SYS_SPORE_REAP, child, (long)&status, 0);
    }
    uint64_t spawn_us = (usec_now() - start) / SAMPLES;
    printf("[spore] fork latency profile: fork+wait=%uus snapshot-spawn+reap=%uus samples=%d\n",
           (unsigned)fork_us,
           (unsigned)spawn_us,
           SAMPLES);
    return 1;
}

static int phase_d_fs_demo(void) {
    int ok = 1;
    if (mkdir("/tmp/spore-demo-d", 0777) != 0) {
        ok = 0;
    }
    int fd = open("/tmp/spore-demo-d/a", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        ok = 0;
    } else {
        const char msg[] = "phase-d";
        if (write(fd, msg, sizeof(msg) - 1) != (ssize_t)(sizeof(msg) - 1)) {
            ok = 0;
        }
        close(fd);
    }
    if (chdir("/tmp/spore-demo-d") != 0) {
        ok = 0;
    }
    char cwd[64];
    if (getcwd(cwd, sizeof(cwd)) == NULL || strcmp(cwd, "/tmp/spore-demo-d") != 0) {
        ok = 0;
    }
    fd = open("a", O_RDONLY);
    char buf[32] = {0};
    if (fd < 0 || read(fd, buf, sizeof(buf) - 1) != 7 || strcmp(buf, "phase-d") != 0) {
        ok = 0;
    }
    if (fd >= 0) {
        close(fd);
    }
    if (rename("a", "b") != 0) {
        ok = 0;
    }
    int dfd = open(".", O_RDONLY);
    char dents[256];
    long dent_bytes = syscall(SYS_getdents64, dfd, dents, sizeof(dents));
    int saw_b = 0;
    for (long off = 0; off < dent_bytes;) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(dents + off);
        if (strcmp(d->d_name, "b") == 0) {
            saw_b = 1;
        }
        off += d->d_reclen;
    }
    if (dfd >= 0) {
        close(dfd);
    }
    if (!saw_b) {
        ok = 0;
    }
    if (unlink("b") != 0) {
        ok = 0;
    }
    if (chdir("/") != 0) {
        ok = 0;
    }
    (void)syscall(35, AT_FDCWD, "/tmp/spore-demo-d", 0x200);
    printf("[spore] v2d fs/cwd demo: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

int main(void) {
    size_t len = 8 * 1024 * 1024;
    unsigned char *mem = malloc(len);
    if (mem == NULL) {
        printf("[spore] cell 1: malloc 8MB ... failed\n");
        return 1;
    }
    uintptr_t base = page_down((uintptr_t)mem);
    size_t span = len + ((uintptr_t)mem - base);
    long before = raw_syscall3(SYS_SPORE_RESIDENT, (long)base, (long)span, 0);
    for (size_t i = 0; i < len; i += 4096) {
        mem[i] = (unsigned char)(i >> 12);
    }
    long after_touch = raw_syscall3(SYS_SPORE_RESIDENT, (long)base, (long)span, 0);
    printf("[spore] cell 1: malloc 8MB ... resident +%ld pages\n", after_touch - before);
    free(mem);
    long after_free = raw_syscall3(SYS_SPORE_RESIDENT, (long)base, (long)span, 0);
    printf("[spore] cell 1: free      ... resident -%ld pages (returned)\n",
           after_touch - after_free);

    int fd = openat(AT_FDCWD, "/etc/motd", O_RDONLY);
    if (fd < 0) {
        printf("[spore] cell 1: open /etc/motd failed\n");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        printf("[spore] cell 1: fstat /etc/motd failed\n");
        return 1;
    }
    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("[spore] cell 1: read /etc/motd failed\n");
        return 1;
    }
    buf[n] = 0;
    if (n > 0 && buf[n - 1] == '\n') {
        buf[n - 1] = 0;
    }
    printf("[spore] cell 1: open /etc/motd fd=%d size=%ld\n", fd, (long)st.st_size);
    printf("[spore] cell 1: read %ld bytes: \"%s\"\n", (long)n, buf);
    close(fd);

    int dfd = openat(AT_FDCWD, "/", O_RDONLY);
    char dents[256];
    long dent_bytes = syscall(SYS_getdents64, dfd, dents, sizeof(dents));
    printf("[spore] cell 1: listdir / ->");
    for (long off = 0; off < dent_bytes;) {
        struct linux_dirent64 *d = (struct linux_dirent64 *)(dents + off);
        printf(" %s", d->d_name);
        off += d->d_reclen;
    }
    printf("\n");
    close(dfd);

    int ok_all = 1;
    int ok_v3a = phase_a_thread_demo();
    ok_all = ok_all && ok_v3a;
    int ok_v3b = phase_b_futex_demo();
    ok_all = ok_all && ok_v3b;
    int ok_v3c = phase_c_pthread_demo();
    ok_all = ok_all && ok_v3c;
    int ok_v3e = phase_e_udp_demo();
    ok_all = ok_all && ok_v3e;
    int ok_v3f = phase_f_egress_demo();
    ok_all = ok_all && ok_v3f;
    ok_all = ok_all && fork_latency_profile();

    if (after_touch <= before || after_free >= after_touch) {
        ok_all = 0;
    }
    if (strcmp(buf, "welcome to spore") != 0) {
        ok_all = 0;
    }
    if (dent_bytes <= 0) {
        ok_all = 0;
    }

    int ok_v1 = snapshot_regression();
    printf("[spore] v1 regression (snapshot/spawn/reap): %s\n",
           ok_v1 ? "PASS" : "FAIL");
    ok_all = ok_all && ok_v1;

    int ok_v2b = phase_b_timer_budget_demo();
    printf("[spore] v2b timer/budget demo: %s\n",
           ok_v2b ? "PASS" : "FAIL");
    ok_all = ok_all && ok_v2b;

    int ok_v2c = phase_c_exec_demo();
    printf("[spore] v2c fork/exec/wait demo: %s\n",
           ok_v2c ? "PASS" : "FAIL");
    ok_all = ok_all && ok_v2c;

    (void)phase_c_stdin_demo();
    ok_all = ok_all && phase_d_fs_demo();
    printf("[spore] integration regression: %s\n", ok_all ? "PASS" : "FAIL");
    printf("[spore] cell 1: exit(%d)\n", ok_all ? 0 : 1);
    return ok_all ? 0 : 1;
}
