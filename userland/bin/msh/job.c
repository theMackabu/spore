#include "msh.h"

#include <signal.h>
#include <unistd.h>

static pid_t shell_pgrp;

static pid_t current_shell_pgrp(void) {
  if (shell_pgrp <= 0) {
    shell_pgrp = getpgrp();
    if (shell_pgrp <= 0) { shell_pgrp = getpid(); }
  }
  return shell_pgrp;
}

void sh_job_child_setup(pid_t pgrp) {
  if (pgrp <= 0) { pgrp = getpid(); }
  (void)setpgid(0, pgrp);
  (void)signal(SIGINT, SIG_DFL);
  (void)signal(SIGTERM, SIG_DFL);
}

void sh_job_parent_setup(pid_t pid, pid_t pgrp) {
  if (pid <= 0) { return; }
  if (pgrp <= 0) { pgrp = pid; }
  (void)setpgid(pid, pgrp);
}

void sh_job_enter_foreground(pid_t pgrp) {
  if (pgrp > 0) { (void)tcsetpgrp(STDIN_FILENO, pgrp); }
}

void sh_job_leave_foreground(void) {
  (void)tcsetpgrp(STDIN_FILENO, current_shell_pgrp());
}
