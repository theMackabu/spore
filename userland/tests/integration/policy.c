#include "integration_socket.h"

#include <errno.h>
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

int integration_egress_demo(void) {
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
