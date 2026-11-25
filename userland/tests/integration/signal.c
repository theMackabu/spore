#include "integration.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

constexpr int SYS_FUTEX = 98;
constexpr int FUTEX_WAIT_PRIVATE = 128;
constexpr int FUTEX_WAKE_PRIVATE = 129;

uint64_t integration_usec_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static sigjmp_buf probe_return;

static void probe_signal(int sig, siginfo_t *info, void *context) {
  (void)context;
  siglongjmp(probe_return, sig == SIGILL && info->si_code == ILL_ILLOPC && info->si_addr ? 1 : 2);
}

static int vector_probe_fallback(void) {
  if (getauxval(AT_HWCAP) & (1ul << 22)) { return 1; }
  struct sigaction old, action = {.sa_sigaction = probe_signal, .sa_flags = SA_SIGINFO};
  if (sigaction(SIGILL, &action, &old)) { return 0; }
  int caught = sigsetjmp(probe_return, 1);
  if (!caught) { __asm__ volatile(".inst 0x04a03000" ::: "v0"); }
  return sigaction(SIGILL, &old, NULL) == 0 && caught == 1;
}

static volatile sig_atomic_t saw_sigint;
static unsigned char altstack_area[16384];
static volatile uintptr_t signal_stack_addr;
static volatile sig_atomic_t interrupted_signals;
static volatile sig_atomic_t nested_signals;
static volatile sig_atomic_t temporary_mask_signals;
static volatile sig_atomic_t usr2_blocked_in_handler;

static void signal_mask_handler(int signal) {
  saw_sigint += signal;
}

static void signal_altstack_handler(int signal) {
  volatile char marker = (char)signal;
  signal_stack_addr = (uintptr_t)&marker;
}

static void signal_interrupt_handler(int signal) {
  interrupted_signals += signal;
}

static void signal_nested_term_handler(int signal) {
  nested_signals = nested_signals * 10 + signal;
}

static void signal_nested_int_handler(int signal) {
  nested_signals = nested_signals * 10 + signal;
  kill(getpid(), SIGTERM);
  nested_signals = nested_signals * 10 + signal;
}

static void temporary_mask_handler(int signal) {
  sigset_t mask;
  sigprocmask(SIG_SETMASK, NULL, &mask);
  usr2_blocked_in_handler = sigismember(&mask, SIGUSR2);
  temporary_mask_signals += signal;
}

void integration_sleep_ms(long ms) {
  struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

static int interval_timer_regression(void) {
  struct sigaction action = {.sa_handler = signal_interrupt_handler}, old_action;
  struct itimerval timer = {.it_value.tv_usec = 20000, .it_interval.tv_usec = 20000}, previous;
  struct itimerval stop = {0};
  interrupted_signals = 0;
  if (sigaction(SIGALRM, &action, &old_action)) { return 0; }
  int ok = setitimer(ITIMER_REAL, &timer, NULL) == 0;
  for (int i = 0; ok && i < 20 && interrupted_signals < 2 * SIGALRM; ++i) {
    integration_sleep_ms(30);
  }
  ok = setitimer(ITIMER_REAL, &stop, &previous) == 0 && ok && previous.it_interval.tv_usec == 20000 &&
       interrupted_signals >= 2 * SIGALRM;
  sig_atomic_t count = interrupted_signals;
  integration_sleep_ms(60);
  ok = ok && interrupted_signals == count;
  sigaction(SIGALRM, &old_action, NULL);
  printf("[spore] periodic real timer: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int signal_mask_regression(void) {
  saw_sigint = 0;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_mask_handler;
  sigemptyset(&sa.sa_mask);
  int ok = sigaction(SIGINT, &sa, NULL) == 0;

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  ok = ok && sigprocmask(SIG_BLOCK, &set, NULL) == 0;
  ok = ok && kill(getpid(), SIGINT) == 0;
  ok = ok && saw_sigint == 0;
  ok = ok && sigprocmask(SIG_UNBLOCK, &set, NULL) == 0;
  ok = ok && saw_sigint == SIGINT;

  printf("[spore] signal mask pending delivery: %s seen=%d\n", ok ? "PASS" : "FAIL", (int)saw_sigint);
  return ok;
}

static int masked_wait(int kind, int fd, int ep, const sigset_t *mask) {
  struct timespec timeout = {.tv_nsec = 100000000};
  if (kind == 0) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    return ppoll(&pfd, 1, &timeout, mask);
  }
  if (kind == 1) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    return pselect(fd + 1, &readfds, NULL, NULL, &timeout, mask);
  }
  struct epoll_event event;
  return epoll_pwait(ep, &event, 1, 100, mask);
}

static int temporary_mask_regression(void) {
  int fds[2];
  if (pipe(fds) < 0) { return 0; }
  int ep = epoll_create1(0);
  struct epoll_event event = {.events = EPOLLIN, .data.fd = fds[0]};
  struct sigaction action = {.sa_handler = temporary_mask_handler}, old_action;
  sigemptyset(&action.sa_mask);
  sigset_t original, temporary, old_mask;
  sigemptyset(&original);
  sigaddset(&original, SIGUSR1);
  sigemptyset(&temporary);
  sigaddset(&temporary, SIGUSR2);
  if (sigaction(SIGUSR1, &action, &old_action) < 0) {
    close(ep);
    close(fds[0]);
    close(fds[1]);
    return 0;
  }
  int masked = sigprocmask(SIG_SETMASK, &original, &old_mask) == 0;
  int ok = masked && ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &event) == 0;
  for (int kind = 0; ok && kind < 3; ++kind) {
    char byte;
    temporary_mask_signals = 0;
    usr2_blocked_in_handler = 0;
    ok = write(fds[1], "x", 1) == 1 && raise(SIGUSR1) == 0 && masked_wait(kind, fds[0], ep, &temporary) == 1 &&
         temporary_mask_signals == 0 && read(fds[0], &byte, 1) == 1;
    sigset_t after;
    ok = ok && masked_wait(kind, fds[0], ep, &temporary) == -1 && errno == EINTR && temporary_mask_signals == SIGUSR1 &&
         usr2_blocked_in_handler == 1 && sigprocmask(SIG_SETMASK, NULL, &after) == 0 &&
         sigismember(&after, SIGUSR1) == 1 && sigismember(&after, SIGUSR2) == 0;
  }
  if (masked) { sigprocmask(SIG_SETMASK, &old_mask, NULL); }
  sigaction(SIGUSR1, &old_action, NULL);
  if (ep >= 0) { close(ep); }
  close(fds[0]);
  close(fds[1]);
  printf("[spore] temporary signal masks: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static void *send_suspend_signal(void *target) {
  return (void *)(intptr_t)pthread_kill(*(pthread_t *)target, SIGPWR);
}

static int thread_signal_suspend(void) {
  sigset_t blocked, previous, temporary, after;
  sigemptyset(&blocked);
  sigaddset(&blocked, SIGPWR);
  struct sigaction action = {.sa_handler = temporary_mask_handler, .sa_flags = SA_RESTART}, old_power, old_alarm;
  if (sigaction(SIGPWR, &action, &old_power)) { return 0; }
  if (sigaction(SIGALRM, &action, &old_alarm)) {
    sigaction(SIGPWR, &old_power, NULL);
    return 0;
  }
  int masked = pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0;
  int ok = 0;
  if (masked) {
    temporary = previous;
    sigaddset(&temporary, SIGUSR2);
    sigdelset(&temporary, SIGPWR);
    sigdelset(&temporary, SIGALRM);
    pthread_t self = pthread_self(), sender;
    void *result;
    temporary_mask_signals = usr2_blocked_in_handler = 0;
    if (!pthread_create(&sender, NULL, send_suspend_signal, &self) && !pthread_join(sender, &result) && !result) {
      struct itimerval timer = {.it_value.tv_sec = 1}, stop = {0};
      if (!setitimer(ITIMER_REAL, &timer, NULL)) {
        ok = sigsuspend(&temporary) == -1 && errno == EINTR && temporary_mask_signals == SIGPWR &&
             usr2_blocked_in_handler && !pthread_sigmask(SIG_SETMASK, NULL, &after) && sigismember(&after, SIGPWR);
        setitimer(ITIMER_REAL, &stop, NULL);
      }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
  }
  sigaction(SIGPWR, &old_power, NULL);
  sigaction(SIGALRM, &old_alarm, NULL);
  printf("[spore] thread signal suspend: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int signal_altstack_regression(void) {
  stack_t ss = {.ss_sp = altstack_area, .ss_size = sizeof(altstack_area), .ss_flags = 0};
  int ok = sigaltstack(&ss, NULL) == 0;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_altstack_handler;
  sa.sa_flags = SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  signal_stack_addr = 0;
  ok = ok && sigaction(SIGINT, &sa, NULL) == 0;
  ok = ok && kill(getpid(), SIGINT) == 0;

  uintptr_t start = (uintptr_t)altstack_area;
  uintptr_t end = start + sizeof(altstack_area);
  ok = ok && signal_stack_addr >= start && signal_stack_addr < end;

  printf("[spore] signal altstack delivery: %s addr=%p range=%p-%p\n", ok ? "PASS" : "FAIL", (void *)signal_stack_addr,
         (void *)start, (void *)end);
  return ok;
}

static int signal_interrupted_read_regression(void) {
  int fds[2];
  int ok = pipe(fds) == 0;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_interrupt_handler;
  sigemptyset(&sa.sa_mask);
  interrupted_signals = 0;
  ok = ok && sigaction(SIGINT, &sa, NULL) == 0;

  pid_t parent = getpid();
  pid_t child = ok ? fork() : -1;
  if (child == 0) {
    close(fds[0]);
    integration_sleep_ms(20);
    kill(parent, SIGINT);
    _exit(0);
  }

  char ch = 0;
  errno = 0;
  ssize_t n = ok ? read(fds[0], &ch, 1) : -1;
  ok = ok && n == -1 && errno == EINTR && interrupted_signals == SIGINT;
  if (child > 0) { waitpid(child, NULL, 0); }
  if (fds[0] >= 0) { close(fds[0]); }
  if (fds[1] >= 0) { close(fds[1]); }

  printf("[spore] signal interrupted read EINTR: %s errno=%d seen=%d\n", ok ? "PASS" : "FAIL", errno,
         (int)interrupted_signals);
  return ok;
}

static int signal_restart_read_regression(void) {
  int fds[2];
  int ok = pipe(fds) == 0;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_interrupt_handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  interrupted_signals = 0;
  ok = ok && sigaction(SIGINT, &sa, NULL) == 0;

  pid_t parent = getpid();
  pid_t child = ok ? fork() : -1;
  if (child == 0) {
    close(fds[0]);
    integration_sleep_ms(20);
    kill(parent, SIGINT);
    integration_sleep_ms(20);
    write(fds[1], "R", 1);
    _exit(0);
  }

  char ch = 0;
  errno = 0;
  ssize_t n = ok ? read(fds[0], &ch, 1) : -1;
  ok = ok && n == 1 && ch == 'R' && interrupted_signals == SIGINT;
  if (child > 0) { waitpid(child, NULL, 0); }
  if (fds[0] >= 0) { close(fds[0]); }
  if (fds[1] >= 0) { close(fds[1]); }

  printf("[spore] signal restarted read: %s n=%zd ch=%c seen=%d\n", ok ? "PASS" : "FAIL", n, ch ? ch : '-',
         (int)interrupted_signals);
  return ok;
}

static volatile int futex_restart_word;
static pid_t futex_waiter_tid;

static void *futex_restart_thread(void *arg) {
  (void)arg;
  integration_sleep_ms(20);
  syscall(SYS_tgkill, getpid(), futex_waiter_tid, SIGINT);
  integration_sleep_ms(20);
  futex_restart_word = 1;
  integration_raw_syscall4(SYS_FUTEX, (long)&futex_restart_word, FUTEX_WAKE_PRIVATE, 1, 0);
  return NULL;
}

static int signal_restart_futex_regression(const struct timespec *timeout) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_interrupt_handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  interrupted_signals = 0;
  futex_restart_word = 0;
  futex_waiter_tid = (pid_t)syscall(SYS_gettid);

  pthread_t thread;
  int ok = sigaction(SIGINT, &sa, NULL) == 0 && pthread_create(&thread, NULL, futex_restart_thread, NULL) == 0;
  long rc =
    ok ? integration_raw_syscall4(SYS_FUTEX, (long)&futex_restart_word, FUTEX_WAIT_PRIVATE, 0, (long)timeout) : -1;
  if (ok) { ok = pthread_join(thread, NULL) == 0; }
  ok = ok && rc == (timeout ? -EINTR : 0) && futex_restart_word == 1 && interrupted_signals == SIGINT;
  printf("[spore] signal %s futex wait: %s rc=%ld seen=%d\n", timeout ? "interrupted timed" : "restarted",
         ok ? "PASS" : "FAIL", rc, (int)interrupted_signals);
  return ok;
}

static int signal_nested_regression(void) {
  struct sigaction sa_int;
  memset(&sa_int, 0, sizeof(sa_int));
  sa_int.sa_handler = signal_nested_int_handler;
  sigemptyset(&sa_int.sa_mask);

  struct sigaction sa_term;
  memset(&sa_term, 0, sizeof(sa_term));
  sa_term.sa_handler = signal_nested_term_handler;
  sigemptyset(&sa_term.sa_mask);

  nested_signals = 0;
  int ok = sigaction(SIGINT, &sa_int, NULL) == 0 && sigaction(SIGTERM, &sa_term, NULL) == 0 &&
           kill(getpid(), SIGINT) == 0 && nested_signals == 352;
  printf("[spore] nested signal frames: %s sequence=%d\n", ok ? "PASS" : "FAIL", (int)nested_signals);
  return ok;
}

constexpr int SYS_CLOCK_NANOSLEEP = 115;

static int absolute_sleep_regression(void) {
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_nsec += 1000000;
  if (deadline.tv_nsec >= 1000000000) {
    ++deadline.tv_sec;
    deadline.tv_nsec -= 1000000000;
  }
  long rc = integration_raw_syscall4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, TIMER_ABSTIME, (long)&deadline, 0);
  printf("[spore] absolute clock_nanosleep: %s rc=%ld\n", rc == 0 ? "PASS" : "FAIL", rc);
  return rc == 0;
}

static volatile sig_atomic_t child_notice;

static void child_exited(int sig) {
  child_notice = sig;
}

static int child_signal_notification(void) {
  struct sigaction old, action = {.sa_handler = child_exited};
  if (sigaction(SIGCHLD, &action, &old)) { return 0; }
  child_notice = 0;
  pid_t child = fork();
  if (!child) { _exit(23); }
  uint64_t end = integration_usec_now() + 1000000;
  while (child > 0 && !child_notice && integration_usec_now() < end) {
    usleep(1000);
  }
  int status;
  int ok = child > 0 && child_notice == SIGCHLD && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 23;
  return !sigaction(SIGCHLD, &old, NULL) && ok;
}

static volatile sig_atomic_t death_sender;

static void parent_died(int sig, siginfo_t *info, void *context) {
  (void)context;
  if (sig == SIGUSR1 && info->si_code == SI_USER) { death_sender = info->si_pid; }
}

static void *fork_from_thread(void *arg) {
  pid_t *child = arg;
  int ready[2];
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) || pipe(ready)) { return NULL; }
  *child = fork();
  if (*child == 0) {
    close(ready[0]);
    int value = -1;
    struct sigaction action = {.sa_sigaction = parent_died, .sa_flags = SA_SIGINFO};
    pid_t parent = getppid();
    if (prctl(PR_GET_PDEATHSIG, &value) || value || sigaction(SIGUSR1, &action, NULL) ||
        prctl(PR_SET_PDEATHSIG, SIGUSR1) || prctl(PR_SET_PDEATHSIG, 65) != -1 || errno != EINVAL ||
        prctl(PR_GET_PDEATHSIG, &value) || value != SIGUSR1 || write(ready[1], ".", 1) != 1) {
      _exit(1);
    }
    uint64_t end = integration_usec_now() + 1000000;
    while (!death_sender && integration_usec_now() < end) {
      usleep(1000);
    }
    if (death_sender != parent || getppid() != parent) { _exit(2); }
    execl("/bin/exec_child", "exec_child", "parent-death-signal", (char *)NULL);
    _exit(3);
  }
  close(ready[1]);
  char byte;
  if (*child > 0) { (void)read(ready[0], &byte, 1); }
  close(ready[0]);
  return NULL;
}

static int orphan_notification(void) {
  int report[2];
  if (pipe(report)) { return 0; }
  pid_t parent = fork();
  if (parent == 0) {
    close(report[0]);
    int ready[2];
    if (pipe(ready)) { _exit(1); }
    pid_t creator = getpid();
    pid_t child = fork();
    if (child == 0) {
      close(ready[0]);
      sigset_t blocked;
      sigemptyset(&blocked);
      sigaddset(&blocked, SIGUSR1);
      struct sigaction action = {.sa_sigaction = parent_died, .sa_flags = SA_SIGINFO};
      if (sigaction(SIGUSR1, &action, NULL) || sigprocmask(SIG_BLOCK, &blocked, NULL) ||
          prctl(PR_SET_PDEATHSIG, SIGUSR1) || write(ready[1], ".", 1) != 1) {
        _exit(1);
      }
      uint64_t end = integration_usec_now() + 1000000;
      while (getppid() == creator && integration_usec_now() < end) {
        usleep(1000);
      }
      if (getppid() == 1 && !sigprocmask(SIG_UNBLOCK, &blocked, NULL) && death_sender == creator) {
        pid_t self = getpid();
        (void)write(report[1], &self, sizeof(self));
      }
      _exit(0);
    }
    close(ready[1]);
    char byte;
    _exit(child > 0 && read(ready[0], &byte, 1) == 1 ? 0 : 1);
  }
  close(report[1]);
  pid_t adopted;
  int status;
  int ok = parent > 0 && read(report[0], &adopted, sizeof(adopted)) == sizeof(adopted) &&
           waitpid(adopted, &status, 0) == adopted && status == 0;
  if (parent > 0 && (waitpid(parent, &status, 0) != parent || status != 0)) { ok = 0; }
  close(report[0]);
  return ok;
}

static int parent_death_notification(void) {
  pid_t child = -1;
  pthread_t creator;
  int status;
  int ok = pthread_create(&creator, NULL, fork_from_thread, &child) == 0 && pthread_join(creator, NULL) == 0 &&
           child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status);
  ok = ok && orphan_notification();
  printf("[spore] parent death notification: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int signalfd_child_exit(void) {
  sigset_t mask, old;
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  if (sigprocmask(SIG_BLOCK, &mask, &old)) { return 0; }
  int fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  struct signalfd_siginfo info;
  pid_t child = fd < 0 ? -1 : fork();
  if (!child) { _exit(37); }
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int status;
  int ok = child > 0 && poll(&pfd, 1, 1500) == 1 && (pfd.revents & POLLIN) &&
           read(fd, &info, sizeof(info)) == sizeof(info) && info.ssi_signo == SIGCHLD && info.ssi_code == CLD_EXITED &&
           info.ssi_pid == (unsigned)child && info.ssi_status == 37 && read(fd, &info, sizeof(info)) == -1 &&
           errno == EAGAIN;
  if (child > 0) { ok &= waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 37; }
  if (fd >= 0) { close(fd); }
  sigprocmask(SIG_SETMASK, &old, NULL);
  printf("[spore] signalfd child exit and wait: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

int integration_signal_suite(void) {
  struct timespec timeout = {.tv_sec = 1};
  return signalfd_child_exit() && parent_death_notification() && child_signal_notification() &&
         vector_probe_fallback() && absolute_sleep_regression() && interval_timer_regression() &&
         signal_mask_regression() && temporary_mask_regression() && thread_signal_suspend() &&
         signal_altstack_regression() && signal_interrupted_read_regression() && signal_restart_read_regression() &&
         signal_restart_futex_regression(&timeout) && signal_restart_futex_regression(NULL) &&
         signal_nested_regression();
}
