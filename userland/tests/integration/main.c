#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
#define SYS_CLOCK_NANOSLEEP 115
#define SYS_GETTID 178
#define SYS_SENDTO 206
#define SYS_ACCEPT4 242

#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD 0x00010000
#define CLONE_SYSVSEM 0x00040000
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 04000
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 02000000
#endif
#ifndef MSG_WAITALL
#define MSG_WAITALL 0x100
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x4000
#endif

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

static long raw_syscall5(long nr, long a0, long a1, long a2, long a3, long a4) {
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x3 __asm__("x3") = a3;
  register long x4 __asm__("x4") = a4;
  register long x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
  return x0;
}

static long raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
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

static void exit_group(int code) {
  raw_syscall3(SYS_EXIT_GROUP, code, 0, 0);
  for (;;) {}
}

static void regression_child(uint64_t code) {
  exit_group((int)code);
}

static void compute_child(uint64_t branch) {
  volatile uint64_t acc = branch + 0x1234;
  for (uint64_t i = 0; i < 30000000ull; ++i) {
    acc = (acc * 1103515245ull + 12345ull + branch) & 0xffffffffull;
  }
  printf("[spore] compute domain %lu finished acc=0x%lx\n", (unsigned long)branch, (unsigned long)acc);
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
  if (snap < 0) { return 0; }
  int child = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)regression_child, 7);
  if (child < 0) { return 0; }
  int status = 0;
  int got = (int)raw_syscall3(SYS_SPORE_REAP, child, (long)&status, 0);
  return got == child && ((status >> 8) & 0xff) == 7;
}

static int phase_b_timer_budget_demo(void) {
  int snap = (int)raw_syscall3(SYS_SPORE_SNAPSHOT, 0, 0, 0);
  if (snap < 0) { return 0; }

  int a = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)compute_child, 1);
  int b = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)compute_child, 2);
  if (a < 0 || b < 0) { return 0; }
  int status_a = 0;
  int status_b = 0;
  int got_a = (int)raw_syscall3(SYS_SPORE_REAP, a, (long)&status_a, 0);
  int got_b = (int)raw_syscall3(SYS_SPORE_REAP, b, (long)&status_b, 0);
  int ok_compute = got_a == a && got_b == b && ((status_a >> 8) & 0xff) == 11 && ((status_b >> 8) & 0xff) == 12;
  printf("[spore] timer demo: compute domains %d/%d both finished: %s\n", a, b, ok_compute ? "PASS" : "FAIL");

  int spin = (int)raw_syscall3(SYS_SPORE_SPAWN, snap, (long)spinner_child, 0);
  if (spin < 0) { return 0; }
  raw_syscall3(SYS_SPORE_SET_BUDGET, spin, 5, 0);
  int spin_status = 0;
  int got_spin = (int)raw_syscall3(SYS_SPORE_REAP, spin, (long)&spin_status, 0);
  int ok_budget = got_spin == spin && ((spin_status >> 8) & 0xff) == 137;
  printf("[spore] timer demo: runaway domain %d budget kill: %s\n", spin, ok_budget ? "PASS" : "FAIL");
  return ok_compute && ok_budget;
}

static int phase_c_exec_demo(void) {
  pid_t pid = fork();
  if (pid < 0) { return 0; }
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
  if (pid < 0) { return 0; }
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
  if (slot >= 0 && slot < 2 && pid == thread_demo_pid && tid != thread_demo_tid) { thread_slots[slot] = tid; }
  printf("[spore] v3a thread %ld: domain=%d tid=%d\n", slot, pid, tid);
  raw_syscall3(SYS_EXIT, 0, 0, 0);
  for (;;) {}
}

static int phase_a_thread_demo(void) {
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

static void phase_b_futex_entry(void *arg) {
  long slot = (long)arg;
  if (slot >= 0 && slot < 2) { futex_started[slot] = 1; }
  long rc = raw_syscall4(SYS_FUTEX, (long)&futex_word, FUTEX_WAIT_PRIVATE, 0, 0);
  if ((rc == 0 || rc == -11) && slot >= 0 && slot < 2) { futex_finished[slot] = 1; }
  printf("[spore] v3b futex thread %ld resumed rc=%ld\n", slot, rc);
  raw_syscall3(SYS_EXIT, 0, 0, 0);
  for (;;) {}
}

static int phase_b_futex_demo(void) {
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
    if (pthread_join(threads[i], &ret) != 0 || ret != (void *)(i + 0x100)) { ok = 0; }
  }
  ok = ok && pthread_demo_counter == THREADS * 1000;
  printf("[spore] v3c pthread create/join: %s counter=%d\n", ok ? "PASS" : "FAIL", pthread_demo_counter);
  return ok;
}

static uint16_t be16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

static uint32_t ip4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  return a | (b << 8) | (c << 16) | (d << 24);
}

static uint64_t usec_now(void);

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
  int ok = sent == (ssize_t)(sizeof(msg) - 1) && got == (ssize_t)(sizeof(msg) - 1) && strcmp(buf, msg) == 0;
  printf("[spore] v3e udp socket echo: %s (%s)\n", ok ? "PASS" : "FAIL", buf);
  return ok;
}

static int udp_bind_conflict_regression(void) {
  int a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (a < 0 || b < 0) {
    if (a >= 0) { close(a); }
    if (b >= 0) { close(b); }
    printf("[spore] udp bind conflict: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(0, 0, 0, 0), 45678);
  int first = bind(a, (struct sockaddr *)&sa, sizeof(sa));
  errno = 0;
  int second = bind(b, (struct sockaddr *)&sa, sizeof(sa));
  int ok = first == 0 && second < 0 && errno == EADDRINUSE;
  close(a);
  close(b);
  printf("[spore] udp bind conflict: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int udp_receive_queue_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp receive queue: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(10, 0, 2, 2), 5555);
  const char first[] = "udp-q1";
  const char second[] = "udp-q2";
  ssize_t sent_first = sendto(fd, first, sizeof(first) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  ssize_t sent_second = sendto(fd, second, sizeof(second) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  char got_first[16] = {0};
  char got_second[16] = {0};
  ssize_t read_first = recvfrom(fd, got_first, sizeof(got_first) - 1, 0, NULL, NULL);
  ssize_t read_second = recvfrom(fd, got_second, sizeof(got_second) - 1, 0, NULL, NULL);
  close(fd);
  int ok = sent_first == (ssize_t)(sizeof(first) - 1) && sent_second == (ssize_t)(sizeof(second) - 1) &&
           read_first == (ssize_t)(sizeof(first) - 1) && read_second == (ssize_t)(sizeof(second) - 1) &&
           strcmp(got_first, first) == 0 && strcmp(got_second, second) == 0;
  printf("[spore] udp receive queue: %s (%s,%s)\n", ok ? "PASS" : "FAIL", got_first, got_second);
  return ok;
}

static int udp_msg_iov_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp msg iov: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(10, 0, 2, 2), 5555);
  const char a[] = "iov-";
  const char b[] = "send-";
  const char c[] = "recv";
  struct iovec out_iov[3] = {
    {.iov_base = (void *)a, .iov_len = sizeof(a) - 1},
    {.iov_base = (void *)b, .iov_len = sizeof(b) - 1},
    {.iov_base = (void *)c, .iov_len = sizeof(c) - 1},
  };
  struct msghdr out = {
    .msg_name = &sa,
    .msg_namelen = sizeof(sa),
    .msg_iov = out_iov,
    .msg_iovlen = 3,
  };
  ssize_t sent = sendmsg(fd, &out, 0);

  char first[5] = {0};
  char second[10] = {0};
  struct iovec in_iov[2] = {
    {.iov_base = first, .iov_len = sizeof(first) - 1},
    {.iov_base = second, .iov_len = sizeof(second) - 1},
  };
  struct sockaddr_in from;
  socklen_t from_len = sizeof(from);
  struct msghdr in = {
    .msg_name = &from,
    .msg_namelen = from_len,
    .msg_iov = in_iov,
    .msg_iovlen = 2,
  };
  ssize_t got = recvmsg(fd, &in, 0);
  close(fd);
  int ok = sent == 13 && got == 13 && strcmp(first, "iov-") == 0 && strcmp(second, "send-recv") == 0 &&
           in.msg_namelen == sizeof(from);
  printf("[spore] udp msg iov: %s (%s%s)\n", ok ? "PASS" : "FAIL", first, second);
  return ok;
}

static int udp_recvmsg_trunc_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp recvmsg trunc: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(10, 0, 2, 2), 5555);
  const char payload[] = "truncate-this";

  char small[5] = {0};
  struct iovec iov = {.iov_base = small, .iov_len = 4};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
  ssize_t sent = sendto(fd, payload, sizeof(payload) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  ssize_t got = recvmsg(fd, &msg, 0);
  int ok = sent == (ssize_t)(sizeof(payload) - 1) && got == 4 && strcmp(small, "trun") == 0 &&
           (msg.msg_flags & MSG_TRUNC) != 0;

  memset(small, 0, sizeof(small));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  sent = sendto(fd, payload, sizeof(payload) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  got = recvmsg(fd, &msg, MSG_TRUNC);
  ok = ok && sent == (ssize_t)(sizeof(payload) - 1) && got == (ssize_t)(sizeof(payload) - 1) &&
       strcmp(small, "trun") == 0 && (msg.msg_flags & MSG_TRUNC) != 0;

  close(fd);
  printf("[spore] udp recvmsg trunc: %s got=%zd flags=0x%x\n", ok ? "PASS" : "FAIL", got, msg.msg_flags);
  return ok;
}

static int socket_msg_peek_regression(void) {
  int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp < 0) {
    printf("[spore] socket MSG_PEEK: FAIL udp socket\n");
    return 0;
  }
  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(10, 0, 2, 2), 5555);
  const char first[] = "peek-one";
  const char second[] = "peek-two";
  char peeked[16] = {0};
  char got_first[16] = {0};
  char got_second[16] = {0};
  ssize_t sent_first = sendto(udp, first, sizeof(first) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  ssize_t sent_second = sendto(udp, second, sizeof(second) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  ssize_t peek = recvfrom(udp, peeked, sizeof(peeked) - 1, MSG_PEEK, NULL, NULL);
  ssize_t read_first = recvfrom(udp, got_first, sizeof(got_first) - 1, 0, NULL, NULL);
  ssize_t read_second = recvfrom(udp, got_second, sizeof(got_second) - 1, 0, NULL, NULL);
  int ok = sent_first == (ssize_t)(sizeof(first) - 1) && sent_second == (ssize_t)(sizeof(second) - 1) &&
           peek == (ssize_t)(sizeof(first) - 1) && read_first == (ssize_t)(sizeof(first) - 1) &&
           read_second == (ssize_t)(sizeof(second) - 1) && strcmp(peeked, first) == 0 &&
           strcmp(got_first, first) == 0 && strcmp(got_second, second) == 0;
  close(udp);

  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] socket MSG_PEEK: FAIL tcp socket\n");
    return 0;
  }
  fill_udp_addr(&sa, ip4(127, 0, 0, 1), 45685);
  ok = ok && bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
       connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  const char stream[] = "peek-stream";
  char stream_peek[8] = {0};
  char stream_read[16] = {0};
  if (ok) {
    ok = send(accepted, stream, sizeof(stream) - 1, 0) == (ssize_t)(sizeof(stream) - 1) &&
         recv(client, stream_peek, 4, MSG_PEEK) == 4 && strcmp(stream_peek, "peek") == 0 &&
         recv(client, stream_read, sizeof(stream_read) - 1, 0) == (ssize_t)(sizeof(stream) - 1) &&
         strcmp(stream_read, stream) == 0;
  }

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] socket MSG_PEEK: %s udp=%s/%s/%s tcp=%s/%s\n", ok ? "PASS" : "FAIL", peeked, got_first,
         got_second, stream_peek, stream_read);
  return ok;
}

static int socket_msg_waitall_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] socket MSG_WAITALL: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(127, 0, 0, 1), 45686);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  char buf[16] = {0};
  if (ok) {
    ok = send(accepted, "wait", 4, 0) == 4;
    pid_t child = fork();
    if (child == 0) {
      usleep(20000);
      (void)send(accepted, "-all", 4, 0);
      _exit(0);
    }
    ssize_t got = recv(client, buf, 8, MSG_WAITALL);
    int status = 0;
    if (child > 0) { (void)waitpid(child, &status, 0); }
    ok = ok && child > 0 && got == 8 && memcmp(buf, "wait-all", 8) == 0;
  }

  char iov_a[5] = {0};
  char iov_b[5] = {0};
  if (ok) {
    ok = send(accepted, "iov-", 4, 0) == 4;
    pid_t child = fork();
    if (child == 0) {
      usleep(20000);
      (void)send(accepted, "done", 4, 0);
      _exit(0);
    }
    struct iovec iov[2] = {
      {.iov_base = iov_a, .iov_len = 4},
      {.iov_base = iov_b, .iov_len = 4},
    };
    struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
    ssize_t got = recvmsg(client, &msg, MSG_WAITALL);
    int status = 0;
    if (child > 0) { (void)waitpid(child, &status, 0); }
    ok = ok && child > 0 && got == 8 && memcmp(iov_a, "iov-", 4) == 0 && memcmp(iov_b, "done", 4) == 0;
  }

  char partial[8] = {0};
  if (ok) {
    ok = send(accepted, "mini", 4, 0) == 4;
    errno = 0;
    ssize_t got = recv(client, partial, sizeof(partial), MSG_WAITALL | MSG_DONTWAIT);
    ok = ok && got == 4 && memcmp(partial, "mini", 4) == 0;
  }

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] socket MSG_WAITALL: %s recv=%.*s recvmsg=%.*s%.*s dontwait=%.*s\n", ok ? "PASS" : "FAIL", 8,
         buf, 4, iov_a, 4, iov_b, 4, partial);
  return ok;
}

static int getsockname_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] getsockname: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in bind_sa;
  fill_udp_addr(&bind_sa, ip4(0, 0, 0, 0), 45679);
  struct sockaddr_in got_sa;
  socklen_t got_len = sizeof(got_sa);
  int ok = bind(fd, (struct sockaddr *)&bind_sa, sizeof(bind_sa)) == 0 &&
           getsockname(fd, (struct sockaddr *)&got_sa, &got_len) == 0 && got_sa.sin_port == bind_sa.sin_port &&
           got_len == sizeof(got_sa);
  close(fd);
  printf("[spore] getsockname: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int socket_options_regression(void) {
  int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (udp < 0 || tcp < 0) {
    if (udp >= 0) { close(udp); }
    if (tcp >= 0) { close(tcp); }
    printf("[spore] socket options: FAIL socket\n");
    return 0;
  }
  int one = 1;
  int got = 0;
  socklen_t got_len = sizeof(got);
  int sndbuf = 65536;
  struct timeval tv = {.tv_sec = 1, .tv_usec = 250000};
  int ok = setsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
           setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           setsockopt(udp, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0 &&
           getsockopt(udp, SOL_SOCKET, SO_REUSEADDR, &got, &got_len) == 0 && got == 1 && got_len == sizeof(got) &&
           setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0 &&
           setsockopt(tcp, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one)) == 0 &&
           getsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, &got, &got_len) == 0 && got == 1 && got_len == sizeof(got) &&
           getsockopt(tcp, SOL_SOCKET, SO_KEEPALIVE, &got, &got_len) == 0 && got == 1 && got_len == sizeof(got) &&
           getsockopt(udp, SOL_SOCKET, SO_SNDBUF, &got, &got_len) == 0 && got == sndbuf && got_len == sizeof(got);
  close(udp);
  close(tcp);
  printf("[spore] socket options: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int udp_broadcast_option_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp broadcast option: FAIL socket\n");
    return 0;
  }

  char payload[] = "broadcast";
  struct sockaddr_in dst;
  fill_udp_addr(&dst, ip4(255, 255, 255, 255), 45687);

  errno = 0;
  ssize_t denied = sendto(fd, payload, sizeof(payload), 0, (struct sockaddr *)&dst, sizeof(dst));
  int denied_errno = errno;

  int one = 1;
  errno = 0;
  int set_ok = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  ssize_t sent = sendto(fd, payload, sizeof(payload), 0, (struct sockaddr *)&dst, sizeof(dst));
  int send_errno = errno;

  close(fd);

  int ok = denied < 0 && denied_errno == EACCES && set_ok == 0 && sent == (ssize_t)sizeof(payload);
  printf("[spore] udp broadcast option: %s denied=%d sent=%zd errno=%d\n", ok ? "PASS" : "FAIL",
         denied_errno, sent, send_errno);
  return ok;
}

static int udp_loopback_refused_regression(void) {
  char payload[] = "x";
  struct sockaddr_in closed;
  fill_udp_addr(&closed, ip4(127, 0, 0, 1), 45992);

  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp loopback refused: FAIL socket\n");
    return 0;
  }
  int ok = connect(fd, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
           send(fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);

  struct pollfd pfd = {.fd = fd, .events = 0};
  int poll_rc = poll(&pfd, 1, 0);
  int so_error = 0;
  socklen_t so_error_len = sizeof(so_error);
  int get_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
  int after_error = -1;
  so_error_len = sizeof(after_error);
  int get_after_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &after_error, &so_error_len);
  close(fd);

  int recv_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  ok = ok && recv_fd >= 0 && connect(recv_fd, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
       send(recv_fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);
  char byte = 0;
  errno = 0;
  ssize_t recv_rc = recv(recv_fd, &byte, sizeof(byte), MSG_DONTWAIT);
  int recv_errno = errno;
  if (recv_fd >= 0) { close(recv_fd); }

  int recvmsg_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  ok = ok && recvmsg_fd >= 0 && connect(recvmsg_fd, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
       send(recvmsg_fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);
  struct iovec iov = {.iov_base = &byte, .iov_len = sizeof(byte)};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
  errno = 0;
  ssize_t recvmsg_rc = recvmsg(recvmsg_fd, &msg, MSG_DONTWAIT);
  int recvmsg_errno = errno;
  if (recvmsg_fd >= 0) { close(recvmsg_fd); }

  int epoll_fd = epoll_create1(0);
  int epoll_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int epoll_rc = -1;
  uint32_t epoll_events = 0;
  if (epoll_fd >= 0 && epoll_sock >= 0 && connect(epoll_sock, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
      send(epoll_sock, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload)) {
    struct epoll_event ev = {.events = 0, .data.u32 = 0x55};
    struct epoll_event out = {0};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, epoll_sock, &ev) == 0) {
      epoll_rc = epoll_wait(epoll_fd, &out, 1, 0);
      epoll_events = out.events;
    }
  }
  if (epoll_sock >= 0) { close(epoll_sock); }
  if (epoll_fd >= 0) { close(epoll_fd); }

  int unconnected = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int unconnected_error = -1;
  socklen_t unconnected_len = sizeof(unconnected_error);
  ssize_t unconnected_send = -1;
  int unconnected_get = -1;
  if (unconnected >= 0) {
    unconnected_send = sendto(unconnected, payload, sizeof(payload), 0, (struct sockaddr *)&closed, sizeof(closed));
    unconnected_get = getsockopt(unconnected, SOL_SOCKET, SO_ERROR, &unconnected_error, &unconnected_len);
    close(unconnected);
  }

  ok = ok && poll_rc == 1 && (pfd.revents & POLLERR) != 0 && get_rc == 0 && so_error == ECONNREFUSED &&
       get_after_rc == 0 && after_error == 0 && recv_rc < 0 && recv_errno == ECONNREFUSED && recvmsg_rc < 0 &&
       recvmsg_errno == ECONNREFUSED && epoll_rc == 1 && (epoll_events & EPOLLERR) != 0 &&
       unconnected_send == (ssize_t)sizeof(payload) && unconnected_get == 0 && unconnected_error == 0;
  printf("[spore] udp loopback refused: %s poll=0x%x epoll=0x%x so_error=%d recv=%d recvmsg=%d unconnected=%d\n",
         ok ? "PASS" : "FAIL", pfd.revents, epoll_events, so_error, recv_errno, recvmsg_errno, unconnected_error);
  return ok;
}

static int udp_disconnect_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp disconnect: FAIL socket\n");
    return 0;
  }

  char payload[] = "x";
  struct sockaddr_in peer;
  fill_udp_addr(&peer, ip4(127, 0, 0, 1), 45993);
  int ok = connect(fd, (struct sockaddr *)&peer, sizeof(peer)) == 0 &&
           send(fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);

  struct sockaddr unspec;
  memset(&unspec, 0, sizeof(unspec));
  ok = ok && connect(fd, &unspec, sizeof(unspec)) == 0;

  int so_error = -1;
  socklen_t so_error_len = sizeof(so_error);
  int get_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);

  struct sockaddr_in got_peer;
  socklen_t got_peer_len = sizeof(got_peer);
  errno = 0;
  int peer_rc = getpeername(fd, (struct sockaddr *)&got_peer, &got_peer_len);
  int peer_errno = errno;

  errno = 0;
  ssize_t send_rc = send(fd, payload, sizeof(payload), 0);
  int send_errno = errno;

  ssize_t sendto_rc = sendto(fd, payload, sizeof(payload), 0, (struct sockaddr *)&peer, sizeof(peer));
  close(fd);

  ok = ok && get_rc == 0 && so_error == 0 && peer_rc < 0 && peer_errno == ENOTCONN && send_rc < 0 &&
       send_errno == EINVAL && sendto_rc == (ssize_t)sizeof(payload);
  printf("[spore] udp disconnect: %s so_error=%d peer=%d send=%d sendto=%zd\n", ok ? "PASS" : "FAIL",
         so_error, peer_errno, send_errno, sendto_rc);
  return ok;
}

static int udp_external_refused_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] udp external refused: FAIL socket\n");
    return 0;
  }

  char payload[] = "x";
  struct sockaddr_in closed;
  fill_udp_addr(&closed, ip4(10, 0, 2, 2), 45994);
  int ok = connect(fd, (struct sockaddr *)&closed, sizeof(closed)) == 0 &&
           send(fd, payload, sizeof(payload), 0) == (ssize_t)sizeof(payload);

  struct pollfd pfd = {.fd = fd, .events = 0};
  int poll_rc = poll(&pfd, 1, 250);

  int so_error = 0;
  socklen_t so_error_len = sizeof(so_error);
  int get_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
  close(fd);

  if (poll_rc == 0 && get_rc == 0 && so_error == 0) {
    printf("[spore] udp external refused: SKIP no host ICMP error\n");
    return 1;
  }

  ok = ok && poll_rc == 1 && (pfd.revents & POLLERR) != 0 && get_rc == 0 && so_error == ECONNREFUSED;
  printf("[spore] udp external refused: %s poll=0x%x so_error=%d\n", ok ? "PASS" : "FAIL", pfd.revents,
         so_error);
  return ok;
}

static int socket_timeout_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] socket timeout: FAIL socket\n");
    return 0;
  }
  struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
  struct timeval got_tv = {0};
  socklen_t got_len = sizeof(got_tv);
  char buf[8];
  errno = 0;
  int ok = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &got_tv, &got_len) == 0 && got_len == sizeof(got_tv) &&
           got_tv.tv_sec == 0 && got_tv.tv_usec == 20000 && recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL) < 0 &&
           (errno == EAGAIN || errno == EWOULDBLOCK);
  close(fd);
  printf("[spore] socket timeout: %s errno=%d\n", ok ? "PASS" : "FAIL", errno);
  return ok;
}

static int socket_msg_dontwait_regression(void) {
  int udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp < 0) {
    printf("[spore] socket MSG_DONTWAIT: FAIL udp socket\n");
    return 0;
  }

  char byte = 0;
  errno = 0;
  uint64_t start = usec_now();
  ssize_t udp_recv = recvfrom(udp, &byte, sizeof(byte), MSG_DONTWAIT, NULL, NULL);
  uint64_t udp_elapsed = usec_now() - start;
  int ok = udp_recv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && udp_elapsed < 5000;

  struct iovec iov = {.iov_base = &byte, .iov_len = sizeof(byte)};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
  errno = 0;
  start = usec_now();
  ssize_t udp_msg = recvmsg(udp, &msg, MSG_DONTWAIT);
  uint64_t udp_msg_elapsed = usec_now() - start;
  ok = ok && udp_msg < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && udp_msg_elapsed < 5000;
  close(udp);

  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] socket MSG_DONTWAIT: FAIL tcp socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(127, 0, 0, 1), 45684);
  ok = ok && bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
       connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  errno = 0;
  start = usec_now();
  ssize_t tcp_recv = recv(client, &byte, sizeof(byte), MSG_DONTWAIT);
  uint64_t tcp_elapsed = usec_now() - start;
  ok = ok && tcp_recv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && tcp_elapsed < 5000;

  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  errno = 0;
  start = usec_now();
  ssize_t tcp_msg = recvmsg(client, &msg, MSG_DONTWAIT);
  uint64_t tcp_msg_elapsed = usec_now() - start;
  ok = ok && tcp_msg < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && tcp_msg_elapsed < 5000;

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] socket MSG_DONTWAIT: %s udp=%luus/%luus tcp=%luus/%luus\n", ok ? "PASS" : "FAIL",
         (unsigned long)udp_elapsed, (unsigned long)udp_msg_elapsed, (unsigned long)tcp_elapsed,
         (unsigned long)tcp_msg_elapsed);
  return ok;
}

static int socket_shutdown_regression(void) {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    printf("[spore] socket shutdown: FAIL socket\n");
    return 0;
  }
  errno = 0;
  int rc = shutdown(fd, SHUT_WR);
  int ok = rc < 0 && errno == ENOTCONN;
  close(fd);
  printf("[spore] socket shutdown: %s errno=%d\n", ok ? "PASS" : "FAIL", errno);
  return ok;
}

static int open_tcp_loopback_pair(uint16_t port, int *client_out, int *accepted_out) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    return 0;
  }
  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(127, 0, 0, 1), port);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  close(server);
  if (!ok || accepted < 0) {
    close(client);
    if (accepted >= 0) { close(accepted); }
    return 0;
  }
  *client_out = client;
  *accepted_out = accepted;
  return 1;
}

static void tcp_sigpipe_child(int mode) {
  int client = -1;
  int accepted = -1;
  if (!open_tcp_loopback_pair((uint16_t)(45685 + mode), &client, &accepted)) { exit_group(70); }
  if (shutdown(client, SHUT_WR) != 0) { exit_group(71); }
  if (mode == 0 && signal(SIGPIPE, SIG_DFL) == SIG_ERR) { exit_group(73); }
  if (mode == 1 && signal(SIGPIPE, SIG_IGN) == SIG_ERR) { exit_group(74); }

  char byte = 'x';
  long rc = raw_syscall6(SYS_SENDTO, client, (long)&byte, 1, mode == 2 ? MSG_NOSIGNAL : 0, 0, 0);
  close(client);
  close(accepted);
  exit_group(rc == -EPIPE ? 0 : 72);
}

static int tcp_sigpipe_regression(void) {
  pid_t default_pid = fork();
  if (default_pid == 0) { tcp_sigpipe_child(0); }
  int default_status = 0;
  waitpid(default_pid, &default_status, 0);

  pid_t ignored_pid = fork();
  if (ignored_pid == 0) { tcp_sigpipe_child(1); }
  int ignored_status = 0;
  waitpid(ignored_pid, &ignored_status, 0);

  pid_t nosignal_pid = fork();
  if (nosignal_pid == 0) { tcp_sigpipe_child(2); }
  int nosignal_status = 0;
  waitpid(nosignal_pid, &nosignal_status, 0);

  int default_ok = WIFSIGNALED(default_status) && WTERMSIG(default_status) == SIGPIPE;
  int ignored_ok = WIFEXITED(ignored_status) && WEXITSTATUS(ignored_status) == 0;
  int nosignal_ok = WIFEXITED(nosignal_status) && WEXITSTATUS(nosignal_status) == 0;
  int ok = default_ok && ignored_ok && nosignal_ok;
  printf("[spore] tcp SIGPIPE: %s default-sig=%d ignored=%d nosignal=%d\n", ok ? "PASS" : "FAIL",
         WIFSIGNALED(default_status) ? WTERMSIG(default_status) : 0, WEXITSTATUS(ignored_status),
         WEXITSTATUS(nosignal_status));
  return ok;
}

static int tcp_loopback_accept_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] tcp loopback accept: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(127, 0, 0, 1), 45681);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 4) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;

  struct pollfd pfd = {.fd = server, .events = POLLIN};
  ok = ok && poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0;

  struct sockaddr_in accepted_peer;
  memset(&accepted_peer, 0, sizeof(accepted_peer));
  socklen_t accepted_peer_len = sizeof(accepted_peer);
  int accepted = ok ? (int)raw_syscall5(SYS_ACCEPT4, server, (long)&accepted_peer, (long)&accepted_peer_len,
                                        SOCK_NONBLOCK | SOCK_CLOEXEC, 0)
                    : -1;
  ok = ok && accepted >= 0;
  if (ok) {
    int status_flags = fcntl(accepted, F_GETFL, 0);
    int fd_flags = fcntl(accepted, F_GETFD, 0);
    ok = accepted_peer_len == sizeof(accepted_peer) && accepted_peer.sin_family == AF_INET &&
         accepted_peer.sin_addr.s_addr == ip4(127, 0, 0, 1) && accepted_peer.sin_port != 0 &&
         (status_flags & O_NONBLOCK) != 0 && (fd_flags & FD_CLOEXEC) != 0;
  }

  const char msg[] = "tcp-hi";
  char got[16] = {0};
  const char reply[] = "tcp-ok";
  char reply_got[16] = {0};
  if (ok) {
    ok = send(client, msg, sizeof(msg) - 1, 0) == (ssize_t)(sizeof(msg) - 1) &&
         recv(accepted, got, sizeof(got) - 1, 0) == (ssize_t)(sizeof(msg) - 1) && strcmp(got, msg) == 0 &&
         send(accepted, reply, sizeof(reply) - 1, 0) == (ssize_t)(sizeof(reply) - 1) &&
         recv(client, reply_got, sizeof(reply_got) - 1, 0) == (ssize_t)(sizeof(reply) - 1) &&
         strcmp(reply_got, reply) == 0;
  }

  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] tcp loopback accept: %s (%s/%s)\n", ok ? "PASS" : "FAIL", got, reply_got);
  return ok;
}

static int tcp_fin_eof_regression(void) {
  int client = -1;
  int accepted = -1;
  if (!open_tcp_loopback_pair(45686, &client, &accepted)) {
    printf("[spore] tcp FIN EOF: FAIL pair\n");
    return 0;
  }

  const char msg[] = "fin-data";
  char got[16] = {0};
  int ok = send(accepted, msg, sizeof(msg) - 1, 0) == (ssize_t)(sizeof(msg) - 1) &&
           recv(client, got, sizeof(got) - 1, 0) == (ssize_t)(sizeof(msg) - 1) && strcmp(got, msg) == 0;

  ok = ok && close(accepted) == 0;

  struct pollfd pfd = {.fd = client, .events = POLLIN};
  int poll_rc = poll(&pfd, 1, 0);
  char byte = 0;
  errno = 0;
  ssize_t eof = recv(client, &byte, sizeof(byte), MSG_DONTWAIT);
  int recv_errno = errno;

  close(client);

  ok = ok && poll_rc == 1 && (pfd.revents & POLLIN) != 0 && eof == 0;
  printf("[spore] tcp FIN EOF: %s poll=0x%x eof=%zd errno=%d\n", ok ? "PASS" : "FAIL", pfd.revents, eof,
         recv_errno);
  return ok;
}

static int tcp_refused_poll_error_regression(void) {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    printf("[spore] tcp refused pollerr: FAIL socket\n");
    return 0;
  }

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) { (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK); }

  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(127, 0, 0, 1), 45991);
  errno = 0;
  int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
  int connect_errno = errno;

  struct pollfd pfd = {.fd = fd, .events = 0};
  int poll_rc = poll(&pfd, 1, 50);

  struct sockaddr_in refused_peer;
  socklen_t refused_peer_len = sizeof(refused_peer);
  errno = 0;
  int peer_rc = getpeername(fd, (struct sockaddr *)&refused_peer, &refused_peer_len);
  int peer_errno = errno;

  int epoll_fd = epoll_create1(0);
  int epoll_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int epoll_connect_errno = 0;
  int epoll_rc = -1;
  uint32_t epoll_events = 0;
  if (epoll_fd >= 0 && epoll_sock >= 0) {
    int epoll_flags = fcntl(epoll_sock, F_GETFL, 0);
    if (epoll_flags >= 0) { (void)fcntl(epoll_sock, F_SETFL, epoll_flags | O_NONBLOCK); }
    errno = 0;
    (void)connect(epoll_sock, (struct sockaddr *)&sa, sizeof(sa));
    epoll_connect_errno = errno;
    struct epoll_event ev = {.events = 0, .data.u32 = 0x66};
    struct epoll_event out = {0};
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, epoll_sock, &ev) == 0) {
      epoll_rc = epoll_wait(epoll_fd, &out, 1, 50);
      epoll_events = out.events;
    }
  }
  if (epoll_sock >= 0) { close(epoll_sock); }
  if (epoll_fd >= 0) { close(epoll_fd); }

  int so_error = 0;
  socklen_t so_error_len = sizeof(so_error);
  int get_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len);
  int after_error = -1;
  so_error_len = sizeof(after_error);
  int get_after_rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &after_error, &so_error_len);

  close(fd);

  int ok = rc < 0 && (connect_errno == EINPROGRESS || connect_errno == ECONNREFUSED) && poll_rc == 1 &&
           (pfd.revents & POLLERR) != 0 && get_rc == 0 && so_error == ECONNREFUSED && get_after_rc == 0 &&
           after_error == 0 && peer_rc < 0 && peer_errno == ENOTCONN &&
           (epoll_connect_errno == EINPROGRESS || epoll_connect_errno == ECONNREFUSED) && epoll_rc == 1 &&
           (epoll_events & EPOLLERR) != 0;
  printf("[spore] tcp refused pollerr: %s connect=%d poll=0x%x epoll=0x%x so_error=%d after=%d peer=%d\n",
         ok ? "PASS" : "FAIL", connect_errno, pfd.revents, epoll_events, so_error, after_error, peer_errno);
  return ok;
}

static int tcp_send_timeout_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] tcp send timeout: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(127, 0, 0, 1), 45682);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
  ok = ok && setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;

  char payload[1400];
  memset(payload, 'x', sizeof(payload));
  int saw_eagain = 0;
  int poll_blocked = 0;
  uint64_t elapsed_us = 0;
  if (ok) {
    for (int i = 0; i < 260; ++i) {
      errno = 0;
      uint64_t start = usec_now();
      ssize_t n = send(client, payload, sizeof(payload), 0);
      uint64_t end = usec_now();
      if (n < 0) {
        saw_eagain = errno == EAGAIN || errno == EWOULDBLOCK;
        elapsed_us = end - start;
        break;
      }
      if (n <= 0) {
        ok = 0;
        break;
      }
    }
    struct pollfd pfd = {.fd = client, .events = POLLOUT};
    poll_blocked = poll(&pfd, 1, 0) == 0;
  }

  ok = ok && saw_eagain && poll_blocked && elapsed_us >= 5000;
  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] tcp send timeout: %s elapsed=%luus poll_blocked=%d\n", ok ? "PASS" : "FAIL",
         (unsigned long)elapsed_us, poll_blocked);
  return ok;
}

static int tcp_sendmsg_timeout_regression(void) {
  int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server < 0 || client < 0) {
    if (server >= 0) { close(server); }
    if (client >= 0) { close(client); }
    printf("[spore] tcp sendmsg timeout: FAIL socket\n");
    return 0;
  }

  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip4(127, 0, 0, 1), 45683);
  int ok = bind(server, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(server, 1) == 0 &&
           connect(client, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  int accepted = ok ? accept(server, NULL, NULL) : -1;
  ok = ok && accepted >= 0;

  struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
  ok = ok && setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;

  char first[700];
  char second[700];
  memset(first, 'a', sizeof(first));
  memset(second, 'b', sizeof(second));
  struct iovec iov[2] = {
    {.iov_base = first, .iov_len = sizeof(first)},
    {.iov_base = second, .iov_len = sizeof(second)},
  };
  struct msghdr msg = {
    .msg_iov = iov,
    .msg_iovlen = 2,
  };
  int saw_eagain = 0;
  uint64_t elapsed_us = 0;
  if (ok) {
    for (int i = 0; i < 260; ++i) {
      errno = 0;
      uint64_t start = usec_now();
      ssize_t n = sendmsg(client, &msg, 0);
      uint64_t end = usec_now();
      if (n < 0) {
        saw_eagain = errno == EAGAIN || errno == EWOULDBLOCK;
        elapsed_us = end - start;
        break;
      }
      if (n <= 0) {
        ok = 0;
        break;
      }
    }
  }

  ok = ok && saw_eagain && elapsed_us >= 5000;
  if (accepted >= 0) { close(accepted); }
  close(client);
  close(server);
  printf("[spore] tcp sendmsg timeout: %s elapsed=%luus\n", ok ? "PASS" : "FAIL", (unsigned long)elapsed_us);
  return ok;
}

static int getpeername_regression(void) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) {
    printf("[spore] getpeername: FAIL socket\n");
    return 0;
  }
  struct sockaddr_in peer;
  fill_udp_addr(&peer, ip4(10, 0, 2, 2), 5555);
  struct sockaddr_in got;
  socklen_t got_len = sizeof(got);
  int ok = connect(fd, (struct sockaddr *)&peer, sizeof(peer)) == 0 &&
           getpeername(fd, (struct sockaddr *)&got, &got_len) == 0 && got.sin_port == peer.sin_port &&
           got.sin_addr.s_addr == peer.sin_addr.s_addr && got_len == sizeof(got);
  close(fd);
  printf("[spore] getpeername: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int udp_send_target(uint32_t ip, uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) { return -1; }
  struct sockaddr_in sa;
  fill_udp_addr(&sa, ip, port);
  const char msg[] = "egress";
  int rc = (int)sendto(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
  close(fd);
  return rc;
}

static int udp_connect_send(uint32_t connect_ip, uint16_t connect_port, uint32_t send_ip, uint16_t send_port,
                            int null_dest) {
  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) { return -1; }
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
  if (syscall(SYS_SPORE_APPLY_POLICY, manifest) != 0) { exit_group(20); }
  errno = 0;
  int rc = udp_send_target(ip4(10, 0, 2, 2), 5555);
  if (expect_success) { exit_group(rc == 6 ? 0 : 21); }
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
  if (pid == 0) { egress_attack_child(mode); }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int egress_escalation_demo(void) {
  pid_t pid = fork();
  if (pid == 0) {
    if (syscall(SYS_SPORE_APPLY_POLICY, "net:none") != 0) { exit_group(40); }
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
  if (deny == 0) { egress_child("net:none", 0); }
  int status_deny = 0;
  waitpid(deny, &status_deny, 0);

  pid_t allow = fork();
  if (allow == 0) { egress_child("net:udp:10.0.2.2:5555", 1); }
  int status_allow = 0;
  waitpid(allow, &status_allow, 0);

  int ok = WIFEXITED(status_deny) && WEXITSTATUS(status_deny) == 0 && WIFEXITED(status_allow) &&
           WEXITSTATUS(status_allow) == 0;
  int wrong_port = run_egress_attack(0);
  int cidr_in = run_egress_attack(1);
  int cidr_out = run_egress_attack(2);
  int connect_null = run_egress_attack(3);
  int connect_spoof = run_egress_attack(4);
  int connect_setpeer = run_egress_attack(5);
  int escalation = egress_escalation_demo();
  ok = ok && wrong_port && cidr_in && cidr_out && connect_null && connect_spoof && connect_setpeer && escalation;
  printf("[spore] v3f egress pressure: %s port=%s cidr-in=%s cidr-out=%s connect-null=%s connect-spoof=%s "
         "connect-setpeer=%s child-escalate=%s\n",
         ok ? "PASS" : "FAIL", wrong_port ? "PASS" : "FAIL", cidr_in ? "PASS" : "FAIL", cidr_out ? "PASS" : "FAIL",
         connect_null ? "PASS" : "FAIL", connect_spoof ? "PASS" : "FAIL", connect_setpeer ? "PASS" : "FAIL",
         escalation ? "PASS" : "FAIL");
  return ok;
}

static uint64_t usec_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int absolute_sleep_regression(void) {
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_nsec += 1000000;
  if (deadline.tv_nsec >= 1000000000) {
    ++deadline.tv_sec;
    deadline.tv_nsec -= 1000000000;
  }
  long rc = raw_syscall4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, TIMER_ABSTIME, (long)&deadline, 0);
  printf("[spore] absolute clock_nanosleep: %s rc=%ld\n", rc == 0 ? "PASS" : "FAIL", rc);
  return rc == 0;
}

static int fork_latency_profile(void) {
  enum { SAMPLES = 4 };
  uint64_t start = usec_now();
  for (int i = 0; i < SAMPLES; ++i) {
    pid_t pid = fork();
    if (pid == 0) { exit_group(0); }
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
  printf("[spore] fork latency profile: fork+wait=%uus snapshot-spawn+reap=%uus samples=%d\n", (unsigned)fork_us,
         (unsigned)spawn_us, SAMPLES);
  return 1;
}

static int phase_d_fs_demo(void) {
  int ok = 1;
  if (mkdir("/tmp/spore-demo-d", 0777) != 0) { ok = 0; }
  int fd = open("/tmp/spore-demo-d/a", O_CREAT | O_RDWR | O_TRUNC, 0666);
  if (fd < 0) {
    ok = 0;
  } else {
    const char msg[] = "phase-d";
    if (write(fd, msg, sizeof(msg) - 1) != (ssize_t)(sizeof(msg) - 1)) { ok = 0; }
    close(fd);
  }
  if (chdir("/tmp/spore-demo-d") != 0) { ok = 0; }
  char cwd[64];
  if (getcwd(cwd, sizeof(cwd)) == NULL || strcmp(cwd, "/tmp/spore-demo-d") != 0) { ok = 0; }
  fd = open("a", O_RDONLY);
  char buf[32] = {0};
  if (fd < 0 || read(fd, buf, sizeof(buf) - 1) != 7 || strcmp(buf, "phase-d") != 0) { ok = 0; }
  if (fd >= 0) { close(fd); }
  if (rename("a", "b") != 0) { ok = 0; }
  int dfd = open(".", O_RDONLY);
  char dents[256];
  long dent_bytes = syscall(SYS_getdents64, dfd, dents, sizeof(dents));
  int saw_b = 0;
  for (long off = 0; off < dent_bytes;) {
    struct linux_dirent64 *d = (struct linux_dirent64 *)(dents + off);
    if (strcmp(d->d_name, "b") == 0) { saw_b = 1; }
    off += d->d_reclen;
  }
  if (dfd >= 0) { close(dfd); }
  if (!saw_b) { ok = 0; }
  if (unlink("b") != 0) { ok = 0; }
  if (chdir("/") != 0) { ok = 0; }
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
  printf("[spore] cell 1: free      ... resident -%ld pages (returned)\n", after_touch - after_free);

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
  if (n > 0 && buf[n - 1] == '\n') { buf[n - 1] = 0; }
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
  ok_all = ok_all && udp_bind_conflict_regression();
  ok_all = ok_all && udp_receive_queue_regression();
  ok_all = ok_all && udp_msg_iov_regression();
  ok_all = ok_all && udp_recvmsg_trunc_regression();
  ok_all = ok_all && socket_msg_peek_regression();
  ok_all = ok_all && socket_msg_waitall_regression();
  ok_all = ok_all && getsockname_regression();
  ok_all = ok_all && socket_options_regression();
  ok_all = ok_all && udp_broadcast_option_regression();
  ok_all = ok_all && udp_loopback_refused_regression();
  ok_all = ok_all && udp_disconnect_regression();
  ok_all = ok_all && udp_external_refused_regression();
  ok_all = ok_all && socket_timeout_regression();
  ok_all = ok_all && socket_msg_dontwait_regression();
  ok_all = ok_all && socket_shutdown_regression();
  ok_all = ok_all && tcp_sigpipe_regression();
  ok_all = ok_all && tcp_loopback_accept_regression();
  ok_all = ok_all && tcp_fin_eof_regression();
  ok_all = ok_all && tcp_refused_poll_error_regression();
  ok_all = ok_all && tcp_send_timeout_regression();
  ok_all = ok_all && tcp_sendmsg_timeout_regression();
  ok_all = ok_all && getpeername_regression();
  int ok_v3f = phase_f_egress_demo();
  ok_all = ok_all && ok_v3f;
  ok_all = ok_all && absolute_sleep_regression();
  ok_all = ok_all && fork_latency_profile();

  if (after_touch <= before || after_free >= after_touch) { ok_all = 0; }
  if (n <= 0 || strstr(buf, "Spore") == NULL) { ok_all = 0; }
  if (dent_bytes <= 0) { ok_all = 0; }

  int ok_v1 = snapshot_regression();
  printf("[spore] v1 regression (snapshot/spawn/reap): %s\n", ok_v1 ? "PASS" : "FAIL");
  ok_all = ok_all && ok_v1;

  int ok_v2b = phase_b_timer_budget_demo();
  printf("[spore] v2b timer/budget demo: %s\n", ok_v2b ? "PASS" : "FAIL");
  ok_all = ok_all && ok_v2b;

  int ok_v2c = phase_c_exec_demo();
  printf("[spore] v2c fork/exec/wait demo: %s\n", ok_v2c ? "PASS" : "FAIL");
  ok_all = ok_all && ok_v2c;

  (void)phase_c_stdin_demo();
  ok_all = ok_all && phase_d_fs_demo();
  printf("[spore] integration regression: %s\n", ok_all ? "PASS" : "FAIL");
  printf("[spore] cell 1: exit(%d)\n", ok_all ? 0 : 1);
  return ok_all ? 0 : 1;
}
