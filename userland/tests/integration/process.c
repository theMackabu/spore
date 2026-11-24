#include "integration.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

constexpr int SYS_SPORE_SNAPSHOT = 0x4000;
constexpr int SYS_SPORE_SPAWN = 0x4001;
constexpr int SYS_SPORE_REAP = 0x4002;
constexpr int SYS_SPORE_SET_BUDGET = 0x4004;
constexpr int SYS_EXIT = 93;
constexpr int SYS_EXIT_GROUP = 94;
constexpr int SYS_FUTEX = 98;
constexpr int SYS_GETTID = 178;
constexpr int FUTEX_WAIT_PRIVATE = 128;
constexpr int FUTEX_WAKE_PRIVATE = 129;

long integration_raw_syscall3(long nr, long a0, long a1, long a2) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

long integration_raw_syscall4(long nr, long a0, long a1, long a2, long a3) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory");
  return x0;
}

long integration_raw_syscall5(long nr, long a0, long a1, long a2, long a3, long a4) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}

long integration_raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x5 __asm__("x5") = a5;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
  return x0;
}

long spore_clone_start(long flags, void *stack_top, int *parent_tid, void *tls, int *child_tid, void (*fn)(void *),
                       void *arg);

__asm__(".text\n"
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

void integration_exit_group(int code) {
  integration_raw_syscall3(SYS_EXIT_GROUP, code, 0, 0);
  for (;;) {}
}

static void regression_child(uint64_t code) {
  integration_exit_group((int)code);
}

static void compute_child(uint64_t branch) {
  volatile uint64_t acc = branch + 0x1234;
  for (uint64_t i = 0; i < 30000000ull; ++i) {
    acc = (acc * 1103515245ull + 12345ull + branch) & 0xffffffffull;
  }
  printf("[spore] compute domain %lu finished acc=0x%lx\n", (unsigned long)branch, (unsigned long)acc);
  integration_exit_group((int)(10 + branch));
}

static void spinner_child(uint64_t unused) {
  (void)unused;
  volatile uint64_t spin = 0;
  for (;;) {
    ++spin;
  }
}

int integration_snapshot_regression(void) {
  int snap = (int)integration_raw_syscall3(SYS_SPORE_SNAPSHOT, 0, 0, 0);
  if (snap < 0) { return 0; }
  int child = (int)integration_raw_syscall3(SYS_SPORE_SPAWN, snap, (long)regression_child, 7);
  if (child < 0) { return 0; }
  int status = 0;
  int got = (int)integration_raw_syscall3(SYS_SPORE_REAP, child, (long)&status, 0);
  return got == child && ((status >> 8) & 0xff) == 7;
}

int integration_timer_budget_demo(void) {
  int snap = (int)integration_raw_syscall3(SYS_SPORE_SNAPSHOT, 0, 0, 0);
  if (snap < 0) { return 0; }

  int a = (int)integration_raw_syscall3(SYS_SPORE_SPAWN, snap, (long)compute_child, 1);
  int b = (int)integration_raw_syscall3(SYS_SPORE_SPAWN, snap, (long)compute_child, 2);
  if (a < 0 || b < 0) { return 0; }
  int status_a = 0;
  int status_b = 0;
  int got_a = (int)integration_raw_syscall3(SYS_SPORE_REAP, a, (long)&status_a, 0);
  int got_b = (int)integration_raw_syscall3(SYS_SPORE_REAP, b, (long)&status_b, 0);
  int ok_compute = got_a == a && got_b == b && ((status_a >> 8) & 0xff) == 11 && ((status_b >> 8) & 0xff) == 12;
  printf("[spore] timer demo: compute domains %d/%d both finished: %s\n", a, b, ok_compute ? "PASS" : "FAIL");

  int spin = (int)integration_raw_syscall3(SYS_SPORE_SPAWN, snap, (long)spinner_child, 0);
  if (spin < 0) { return 0; }
  integration_raw_syscall3(SYS_SPORE_SET_BUDGET, spin, 5, 0);
  int spin_status = 0;
  int got_spin = (int)integration_raw_syscall3(SYS_SPORE_REAP, spin, (long)&spin_status, 0);
  int ok_budget = got_spin == spin && ((spin_status >> 8) & 0xff) == 137;
  printf("[spore] timer demo: runaway domain %d budget kill: %s\n", spin, ok_budget ? "PASS" : "FAIL");
  return ok_compute && ok_budget;
}

int integration_exec_demo(void) {
  pid_t pid = fork();
  if (pid < 0) { return 0; }
  if (pid == 0) {
    char *const argv[] = {"/bin/exec_child", "ok", NULL};
    char *const envp[] = {NULL};
    execve("/bin/exec_child", argv, envp);
    integration_exit_group(99);
  }
  int status = 0;
  pid_t got = waitpid(pid, &status, 0);
  int ok = got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42;
  printf("[spore] execve demo: child exit 42: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int exec_child_status(const char *arg, int expected) {
  pid_t pid = fork();
  if (pid < 0) { return 0; }
  if (pid == 0) {
    char *const argv[] = {"/bin/exec_child", (char *)arg, NULL};
    char *const envp[] = {NULL};
    execve("/bin/exec_child", argv, envp);
    integration_exit_group(99);
  }
  int status = 0;
  return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == expected;
}

static int exec_cmdline(void) {
  pid_t child = fork();
  if (child == 0) {
    execl("/bin/exec_child", "child", "check-cmdline", "two words", "", (char *)NULL);
    _exit(99);
  }
  int status;
  return child > 0 && waitpid(child, &status, 0) == child && status == 0;
}

int integration_exec_private_data(void) {
  int ok = exec_cmdline() && exec_child_status("poison-data", 0) && exec_child_status("check-data", 0);
  printf("[spore] exec private data COW: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static void *check_proc_thread(void *unused) {
  (void)unused;
  int tid = (int)syscall(SYS_GETTID);
  int pid = getpid();
  char expected[64], target[64];
  int len = snprintf(expected, sizeof(expected), "%d/task/%d", pid, tid);
  int ok = tid != pid && readlink("/proc/thread-self", target, sizeof(target)) == len &&
           memcmp(target, expected, len) == 0;
  DIR *tasks = opendir("/proc/self/task");
  int seen = 0;
  if (tasks == NULL) { return NULL; }
  for (struct dirent *entry; (entry = readdir(tasks)) != NULL;) {
    int id = atoi(entry->d_name);
    if (id != tid && id != pid) { continue; }
    struct stat st;
    ok = ok && fstatat(dirfd(tasks), entry->d_name, &st, 0) == 0 && entry->d_ino == st.st_ino && S_ISDIR(st.st_mode);
    seen |= id == tid ? 1 : 2;
  }
  closedir(tasks);
  FILE *f = fopen("/proc/thread-self/status", "r");
  if (f == NULL) { return NULL; }
  char line[256];
  int reported_tid = -1, tgid = -1, threads = -1;
  while (fgets(line, sizeof(line), f)) {
    sscanf(line, "Pid: %d", &reported_tid);
    sscanf(line, "Tgid: %d", &tgid);
    sscanf(line, "Threads: %d", &threads);
  }
  fclose(f);
  return ok && seen == 3 && reported_tid == tid && tgid == pid && threads >= 2 ? (void *)1 : NULL;
}

int integration_proc_self_links(void) {
  pthread_t worker;
  void *result = NULL;
  if (pthread_create(&worker, NULL, check_proc_thread, NULL) != 0 || pthread_join(worker, &result) != 0 || !result) { return 0; }
  char exe[128];
  char cwd[128];
  ssize_t exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  ssize_t cwd_len = readlink("/proc/self/cwd", cwd, sizeof(cwd) - 1);
  if (exe_len >= 0) { exe[exe_len] = '\0'; }
  if (cwd_len >= 0) { cwd[cwd_len] = '\0'; }
  int ok = exe_len > 0 && cwd_len > 0 && strcmp(exe, "/sbin/init") == 0 && cwd[0] == '/';
  printf("[spore] proc self links: %s exe=%s cwd=%s errno=%d\n", ok ? "PASS" : "FAIL", exe_len >= 0 ? exe : "-",
         cwd_len >= 0 ? cwd : "-", errno);
  return ok;
}

int integration_stdin_demo(void) {
  pid_t pid = fork();
  if (pid < 0) { return 0; }
  if (pid == 0) {
    char c = 0;
    printf("[spore] stdin demo: child blocking on read(0)\n");
    ssize_t n = read(0, &c, 1);
    if (n == 1 && c == 'z') {
      printf("[spore] stdin demo: read 'z'\n");
      integration_exit_group(0);
    }
    integration_exit_group(4);
  }

  int status = 0;
  pid_t waited = 0;
  for (int i = 0; i < 100 && waited == 0; ++i) {
    waited = waitpid(pid, &status, WNOHANG);
    if (waited == 0) { usleep(10000); }
  }
  if (waited == 0) {
    kill(pid, SIGKILL);
    waited = waitpid(pid, &status, 0);
  }
  int ok = waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
  if (slot >= 0 && slot < 2 && pid == thread_demo_pid && tid != thread_demo_tid) { thread_slots[slot] = tid; }
  printf("[spore] v3a thread %ld: domain=%d tid=%d\n", slot, pid, tid);
  integration_raw_syscall3(SYS_EXIT, 0, 0, 0);
  for (;;) {}
}

int integration_thread_demo(void) {
  thread_slots[0] = 0;
  thread_slots[1] = 0;
  thread_demo_pid = getpid();
  thread_demo_tid = (int)syscall(SYS_GETTID);
  long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
  long a = spore_clone_start(flags, thread_stack_a + sizeof(thread_stack_a), NULL, NULL, NULL, phase_a_thread_entry,
                             (void *)0);
  long b = spore_clone_start(flags, thread_stack_b + sizeof(thread_stack_b), NULL, NULL, NULL, phase_a_thread_entry,
                             (void *)1);
  if (a < 0 || b < 0) {
    printf("[spore] v3a raw clone threads: FAIL clone=%ld/%ld\n", a, b);
    return 0;
  }
  for (int i = 0; i < 100000 && (thread_slots[0] == 0 || thread_slots[1] == 0); ++i) {
    sched_yield();
  }
  int ok = thread_slots[0] != 0 && thread_slots[1] != 0 && thread_slots[0] != thread_slots[1];
  printf("[spore] v3a raw clone threads: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static volatile int futex_word;
static volatile int futex_started[2];
static volatile int futex_finished[2];
static const struct timespec futex_wake_timeout = {.tv_sec = 1, .tv_nsec = 0};

static void phase_b_futex_entry(void *arg) {
  long slot = (long)arg;
  if (slot >= 0 && slot < 2) { futex_started[slot] = 1; }
  long rc = integration_raw_syscall4(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT_PRIVATE, 0, (long)&futex_wake_timeout);
  if ((rc == 0 || rc == -11) && slot >= 0 && slot < 2) { futex_finished[slot] = 1; }
  printf("[spore] v3b futex thread %ld resumed rc=%ld\n", slot, rc);
  integration_raw_syscall3(SYS_EXIT, 0, 0, 0);
  for (;;) {}
}

int integration_futex_demo(void) {
  futex_word = 0;
  futex_started[0] = futex_started[1] = 0;
  futex_finished[0] = futex_finished[1] = 0;
  long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
  long a =
    spore_clone_start(flags, futex_stack_a + sizeof(futex_stack_a), NULL, NULL, NULL, phase_b_futex_entry, (void *)0);
  long b =
    spore_clone_start(flags, futex_stack_b + sizeof(futex_stack_b), NULL, NULL, NULL, phase_b_futex_entry, (void *)1);
  if (a < 0 || b < 0) {
    printf("[spore] v3b futex wait/wake: FAIL clone=%ld/%ld\n", a, b);
    return 0;
  }
  for (int i = 0; i < 100000 && (futex_started[0] == 0 || futex_started[1] == 0); ++i) {
    sched_yield();
  }
  futex_word = 1;
  long woke = integration_raw_syscall4(SYS_FUTEX, (long)&futex_word, FUTEX_WAKE_PRIVATE, 2, 0);
  for (int i = 0; i < 100000 && (futex_finished[0] == 0 || futex_finished[1] == 0); ++i) {
    sched_yield();
  }
  int ok = futex_finished[0] != 0 && futex_finished[1] != 0 && woke >= 0;
  struct timespec invalid_timeout = {.tv_sec = 0, .tv_nsec = 1000000000};
  struct timespec zero_timeout = {0};
  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 20000000};
  futex_word = 1;
  long eagain = integration_raw_syscall4(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT_PRIVATE, 0, (long)&timeout);
  futex_word = 0;
  long einval = integration_raw_syscall4(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT_PRIVATE, 0, (long)&invalid_timeout);
  long efault = integration_raw_syscall4(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT_PRIVATE, 0, 1);
  long immediate = integration_raw_syscall4(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT_PRIVATE, 0, (long)&zero_timeout);
  uint64_t start = integration_usec_now();
  long timedout = integration_raw_syscall4(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT_PRIVATE, 0, (long)&timeout);
  uint64_t elapsed = integration_usec_now() - start;
  int timeout_ok = eagain == -EAGAIN && einval == -EINVAL && efault == -EFAULT && immediate == -ETIMEDOUT &&
                   timedout == -ETIMEDOUT && elapsed >= 20000;
  ok = ok && timeout_ok;
  printf("[spore] v3b futex timed wait: %s (rc=%ld elapsed=%luus)\n", timeout_ok ? "PASS" : "FAIL", timedout,
         (unsigned long)elapsed);
  printf("[spore] v3b futex wait/wake: %s (woke=%ld)\n", ok ? "PASS" : "FAIL", woke);
  return ok;
}

int integration_pty_demo(void) {
  enum { PTY_BYTES = 8192 };
  static char initial[PTY_BYTES];
  static char extra[PTY_BYTES / 2];
  static char received[PTY_BYTES];
  for (size_t i = 0; i < sizeof(initial); ++i) { initial[i] = (char)(i * 17u + 3u); }
  for (size_t i = 0; i < sizeof(extra); ++i) { extra[i] = (char)(i * 29u + 7u); }

  int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
  int ok = master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0;
  char *path = ok ? ptsname(master) : NULL;
  int slave = path != NULL ? open(path, O_RDWR | O_NOCTTY | O_NONBLOCK) : -1;
  ok = ok && slave >= 0;
  if (ok) {
    ok = write(master, initial, sizeof(initial)) == (ssize_t)sizeof(initial);
    errno = 0;
    ok = ok && write(master, "x", 1) == -1 && errno == EAGAIN;
    ok = ok && read(slave, received, sizeof(extra)) == (ssize_t)sizeof(extra);
    ok = ok && memcmp(received, initial, sizeof(extra)) == 0;
    ok = ok && write(master, extra, sizeof(extra)) == (ssize_t)sizeof(extra);
    ok = ok && read(slave, received, sizeof(received)) == (ssize_t)sizeof(received);
    ok = ok && memcmp(received, initial + sizeof(extra), sizeof(extra)) == 0 &&
         memcmp(received + sizeof(extra), extra, sizeof(extra)) == 0;
    ok = ok && write(slave, "\n", 1) == 1;
    char crlf[2] = {0};
    ok = ok && read(master, crlf, sizeof(crlf)) == (ssize_t)sizeof(crlf) && crlf[0] == '\r' && crlf[1] == '\n';
  }
  if (slave >= 0) { close(slave); }
  if (master >= 0) { close(master); }

  int reopened = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
  ok = ok && reopened >= 0;
  if (reopened >= 0) { close(reopened); }
  printf("[spore] pty wrap/crlf/backpressure/reopen: %s\n", ok ? "PASS" : "FAIL");
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

int integration_pthread_demo(void) {
  constexpr int THREADS = 8;
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
    if (pthread_join(threads[i], &ret) != 0 || ret != (void *)(i + 0x100)) { ok = 0; }
  }
  ok = ok && pthread_demo_counter == THREADS * 1000;
  printf("[spore] v3c pthread create/join: %s counter=%d\n", ok ? "PASS" : "FAIL", pthread_demo_counter);
  return ok;
}

static int fork_latency_profile(void) {
  constexpr int SAMPLES = 4;
  uint64_t start = integration_usec_now();
  for (int i = 0; i < SAMPLES; ++i) {
    pid_t pid = fork();
    if (pid == 0) { integration_exit_group(0); }
    int status = 0;
    waitpid(pid, &status, 0);
  }
  uint64_t fork_us = (integration_usec_now() - start) / SAMPLES;

  int snap = (int)integration_raw_syscall3(SYS_SPORE_SNAPSHOT, 0, 0, 0);
  start = integration_usec_now();
  for (int i = 0; i < SAMPLES; ++i) {
    int child = (int)integration_raw_syscall3(SYS_SPORE_SPAWN, snap, (long)regression_child, 0);
    int status = 0;
    integration_raw_syscall3(SYS_SPORE_REAP, child, (long)&status, 0);
  }
  uint64_t spawn_us = (integration_usec_now() - start) / SAMPLES;
  printf("[spore] fork latency profile: fork+wait=%uus snapshot-spawn+reap=%uus samples=%d\n", (unsigned)fork_us,
         (unsigned)spawn_us, SAMPLES);
  return 1;
}

static int fork_pressure_regression(void) {
  constexpr int CHILDREN = 32;
  pid_t pids[CHILDREN];
  int ok = 1;
  int spawned = 0;
  for (int i = 0; i < CHILDREN; ++i) {
    pid_t pid = fork();
    if (pid == 0) { integration_exit_group(0); }
    if (pid < 0) {
      ok = 0;
      break;
    }
    pids[spawned++] = pid;
  }
  for (int i = 0; i < spawned; ++i) {
    int status = 0;
    pid_t got = waitpid(pids[i], &status, 0);
    if (got != pids[i] || !WIFEXITED(status) || WEXITSTATUS(status) != 0) { ok = 0; }
  }
  printf("[spore] fork pressure: %s children=%d/%d\n", ok ? "PASS" : "FAIL", spawned, CHILDREN);
  return ok;
}

struct waited_child {
  pid_t pid, result;
  int status;
  struct rusage usage;
};

static void *wait_for_child(void *arg) {
  struct waited_child *child = arg;
  child->result = wait4(child->pid, &child->status, 0, &child->usage);
  return NULL;
}

static int64_t usage_usec(struct timeval t) { return (int64_t)t.tv_sec * 1000000 + t.tv_usec; }

static int child_usage_regression(void) {
  struct rusage before, after;
  struct tms clocks;
  if (getrusage(RUSAGE_CHILDREN, &before)) { return 0; }
  pid_t pid = fork();
  if (pid == 0) {
    struct timespec cpu;
    usleep(30000);
    uint64_t deadline = integration_usec_now() + 2000000;
    do {
      for (volatile unsigned i = 0; i < 10000; ++i) {}
      if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) || integration_usec_now() >= deadline) { _exit(1); }
    } while (cpu.tv_sec == 0 && cpu.tv_nsec < 20000000);
    _exit(17);
  }
  if (pid < 0) { return 0; }
  struct waited_child child = {.pid = pid};
  pthread_t waiter;
  if (pthread_create(&waiter, NULL, wait_for_child, &child)) { waitpid(pid, NULL, 0); return 0; }
  pthread_join(waiter, NULL);
  int64_t used = usage_usec(child.usage.ru_utime);
  int ok = child.result == pid && WIFEXITED(child.status) && WEXITSTATUS(child.status) == 17 &&
           used >= 20000 && child.usage.ru_maxrss > 0 && getrusage(RUSAGE_CHILDREN, &after) == 0 &&
           usage_usec(after.ru_utime) - usage_usec(before.ru_utime) == used &&
           times(&clocks) >= 0 && clocks.tms_cutime == usage_usec(after.ru_utime) / 10000;
  printf("[spore] child resource usage: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

int integration_fork_suite(void) {
  return fork_latency_profile() && fork_pressure_regression() && child_usage_regression();
}
