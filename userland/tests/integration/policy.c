#include "integration_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sched.h>
#include <string.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

constexpr int SYS_SPORE_APPLY_POLICY = 0x4005;

static void egress_child(const char *manifest, int expect_success) {
  if (syscall(SYS_SPORE_APPLY_POLICY, manifest) != 0) { integration_exit_group(20); }
  errno = 0;
  int rc = integration_udp_send_target(integration_ip4(10, 0, 2, 2), 5555);
  if (expect_success) { integration_exit_group(rc == 6 ? 0 : 21); }
  integration_exit_group(rc < 0 && errno == EPERM ? 0 : 22);
}

static void egress_attack_child(int mode) {
  long policy = 0;
  int rc = -1;
  switch (mode) {
  case 0:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
    errno = 0;
    rc = integration_udp_send_target(integration_ip4(10, 0, 2, 2), 5556);
    integration_exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 30);
  case 1:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.0/24:5555");
    rc = integration_udp_send_target(integration_ip4(10, 0, 2, 200), 5555);
    integration_exit_group(policy == 0 && rc == 6 ? 0 : 31);
  case 2:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.0/24:5555");
    errno = 0;
    rc = integration_udp_send_target(integration_ip4(10, 0, 3, 2), 5555);
    integration_exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 32);
  case 3:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
    errno = 0;
    rc = integration_udp_connect_send(integration_ip4(10, 0, 2, 2), 5555, 0, 0, 1);
    integration_exit_group(policy == 0 && rc == 5 ? 0 : 33);
  case 4:
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
    errno = 0;
    rc = integration_udp_connect_send(integration_ip4(10, 0, 2, 2), 5555, integration_ip4(10, 0, 3, 2), 5555, 0);
    integration_exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 34);
  case 5: {
    policy = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa;
    integration_fill_udp_addr(&sa, integration_ip4(10, 0, 2, 2), 5556);
    errno = 0;
    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    close(fd);
    integration_exit_group(policy == 0 && rc < 0 && errno == EPERM ? 0 : 35);
  }
  default:
    integration_exit_group(99);
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
    if (syscall(SYS_SPORE_APPLY_POLICY, "net:none") != 0) { integration_exit_group(40); }
    pid_t grandchild = fork();
    if (grandchild == 0) {
      long rc = syscall(SYS_SPORE_APPLY_POLICY, "net:udp:10.0.2.2:5555");
      integration_exit_group(rc < 0 && errno == EPERM ? 0 : 41);
    }
    int status = 0;
    waitpid(grandchild, &status, 0);
    integration_exit_group(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 42);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int check_egress_profile(const char *manifest, int expect_success) {
  pid_t child = fork();
  if (child == 0) { egress_child(manifest, expect_success); }
  int status;
  return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int integration_egress_demo(void) {
  int ok = check_egress_profile("net:none", 0) && check_egress_profile("net:udp:10.0.2.2:5555", 1) &&
           check_egress_profile("net:udp:10.0.2.7/29:5555", 1);
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

static int check_filter(unsigned action) {
  pid_t child = fork();
  if (child == 0) {
    struct sock_filter code[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, action),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = {.len = sizeof(code) / sizeof(code[0]), .filter = code};
    if (setuid(1000) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program)) { _exit(1); }
    if (action == SECCOMP_RET_KILL_PROCESS) { syscall(SYS_getppid); _exit(2); }
    pid_t next = fork();
    if (!next) {
      execl("/bin/exec_child", "exec_child", "filtered", (char *)NULL);
      _exit(3);
    }
    int status;
    _exit(next > 0 && waitpid(next, &status, 0) == next && status == 0 ? 0 : 4);
  }
  int status;
  if (child < 0 || waitpid(child, &status, 0) != child) { return 0; }
  return action == SECCOMP_RET_KILL_PROCESS ? WIFSIGNALED(status) && WTERMSIG(status) == SIGSYS : status == 0;
}

int integration_seccomp(void) {
  int ok = check_filter(SECCOMP_RET_ERRNO | EACCES) && check_filter(SECCOMP_RET_KILL_PROCESS);
  printf("[spore] seccomp syscall denial, inheritance and termination: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


int integration_uts_namespace(void) {
  char before[65], after[65];
  if (gethostname(before, sizeof(before))) { return 0; }
  pid_t child = syscall(SYS_clone, CLONE_NEWUTS | SIGCHLD, 0, 0, 0, 0);
  if (child == 0) {
    if (gethostname(after, sizeof(after)) || strcmp(before, after) || sethostname("private", 7)) { _exit(1); }
    pid_t next = fork();
    if (!next) {
      if (gethostname(after, sizeof(after)) || strcmp(after, "private") || unshare(CLONE_NEWUTS) ||
          sethostname("nested", 6)) { _exit(2); }
      _exit(0);
    }
    int status;
    _exit(next > 0 && waitpid(next, &status, 0) == next && status == 0 &&
          !gethostname(after, sizeof(after)) && !strcmp(after, "private") ? 0 : 3);
  }
  int status;
  int ok = child > 0 && waitpid(child, &status, 0) == child && status == 0 &&
           !gethostname(after, sizeof(after)) && !strcmp(after, before);
  printf("[spore] private UTS hostname and fork inheritance: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

int integration_capabilities(void) {
  pid_t child = fork();
  if (!child) {
    struct __user_cap_header_struct hdr = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2];
    uint32_t bit = 1u << CAP_SYS_ADMIN;
    if (syscall(SYS_capget, &hdr, caps) || !(caps[0].effective & bit) ||
        prctl(PR_CAPBSET_DROP, CAP_SYS_ADMIN, 0, 0, 0) ||
        prctl(PR_CAPBSET_READ, CAP_SYS_ADMIN, 0, 0, 0) != 0) { _exit(1); }
    caps[0].effective &= ~bit;
    caps[0].permitted &= ~bit;
    if (syscall(SYS_capset, &hdr, caps) || unshare(CLONE_NEWUTS) != -1 || errno != EPERM) { _exit(2); }
    caps[0].permitted |= bit;
    if (syscall(SYS_capset, &hdr, caps) != -1 || errno != EPERM) { _exit(3); }
    execl("/bin/exec_child", "exec_child", "cap-bound", (char *)NULL);
    _exit(4);
  }
  int status;
  int ok = child > 0 && waitpid(child, &status, 0) == child && status == 0;
  printf("[spore] capability drop and exec bounding set: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int write_map(const char *path, const char *text) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) { return -1; }
  ssize_t rc = write(fd, text, strlen(text));
  int saved = errno;
  close(fd);
  errno = saved;
  return rc == (ssize_t)strlen(text) ? 0 : -1;
}

int integration_user_namespace(void) {
  pid_t child = fork();
  if (!child) {
    if (setgid(1000) || setuid(1000) || prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        unshare(CLONE_NEWUSER) || getuid() != 65534 || getgid() != 65534) { _exit(1); }
    if (write_map("/proc/self/uid_map", "0 1000 1\n") ||
        write_map("/proc/self/setgroups", "deny\n") || write_map("/proc/self/gid_map", "0 1000 1\n") ||
        getuid() != 0 || getgid() != 0) { _exit(2); }
    if (sethostname("global", 6) != -1 || errno != EPERM || setuid(1000) != -1 || errno != EINVAL) { _exit(3); }
    if (unshare(CLONE_NEWUTS) || sethostname("child", 5)) { _exit(4); }
    struct stat st;
    if (stat("/", &st) || st.st_uid != 65534) { _exit(5); }
    execl("/bin/exec_child", "exec_child", "mapped-user", (char *)NULL);
    _exit(6);
  }
  int status;
  int ok = child > 0 && waitpid(child, &status, 0) == child && status == 0;
  printf("[spore] private user and group mappings: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}
