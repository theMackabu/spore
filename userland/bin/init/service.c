#include "mycelium.h"

void redirect_output(const char *mode, int fd, int fallback_fd) {
  if (mode[0] == '\0' || streq(mode, "journal")) { return; }
  if (streq(mode, "console")) {
    dup2(fallback_fd, fd);
  } else if (streq(mode, "null")) {
    int nullfd = open("/dev/null", O_WRONLY);
    if (nullfd >= 0) {
      dup2(nullfd, fd);
      close(nullfd);
    }
  } else if (strncmp(mode, "file:", 5) == 0) {
    int out = open(mode + 5, O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (out >= 0) {
      dup2(out, fd);
      close(out);
    }
  }
}

void close_inherited_manager_fds(void) {
  /*
   * The kernel is single-core/run-to-completion, but forked services inherit
   * PID 1's accepted AF_UNIX control sockets unless we close them before exec.
   * Keep only stdio after all requested dup2() routing is complete.
   */
  for (int fd = 3; fd < 64; ++fd) {
    close(fd);
  }
}

bool rate_limit_ok(struct unit *unit) {
  time_t now = time(NULL);
  int kept = 0;
  for (int i = 0; i < unit->start_count && i < START_LIMIT_HISTORY; ++i) {
    if (now - unit->starts[i] <= unit->start_limit_interval) { unit->starts[kept++] = unit->starts[i]; }
  }
  unit->start_count = kept;
  if (unit->start_limit_burst > 0 && kept >= unit->start_limit_burst) {
    copy_text(unit->fail_reason, sizeof(unit->fail_reason), "start-limit-hit");
    unit->state = STATE_FAILED;
    journal_append(unit, "start limit hit");
    return false;
  }
  if (unit->start_count < START_LIMIT_HISTORY) { unit->starts[unit->start_count++] = now; }
  return true;
}

bool start_limit_exhausted(struct unit *unit) {
  if (unit->start_limit_burst <= 0) { return false; }
  time_t now = time(NULL);
  int kept = 0;
  for (int i = 0; i < unit->start_count && i < START_LIMIT_HISTORY; ++i) {
    if (now - unit->starts[i] <= unit->start_limit_interval) { unit->starts[kept++] = unit->starts[i]; }
  }
  unit->start_count = kept;
  return kept >= unit->start_limit_burst;
}

int start_unit_name(const char *name, char *err, size_t err_cap);

static bool start_deps(struct unit *unit, char *err, size_t err_cap) {
  for (int i = 0; i < unit->conflicts.count; ++i) {
    struct unit *conflict = unit_by_name(unit->conflicts.items[i]);
    if (conflict != NULL && (conflict->state == STATE_ACTIVE || conflict->state == STATE_ACTIVATING)) {
      snprintf(err, err_cap, "%s conflicts with active %s\n", unit->name, conflict->name);
      return false;
    }
  }
  for (int i = 0; i < unit->requisite.count; ++i) {
    struct unit *requisite = unit_by_name(unit->requisite.items[i]);
    if (requisite == NULL || (requisite->state != STATE_ACTIVE && requisite->state != STATE_ACTIVATING)) {
      snprintf(err, err_cap, "%s requisite %s is not active\n", unit->name, unit->requisite.items[i]);
      return false;
    }
  }
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    if (units[i].used && list_has(&units[i].before, unit->name) &&
        (units[i].state == STATE_INACTIVE || units[i].state == STATE_FAILED)) {
      (void)start_unit_name(units[i].name, err, err_cap);
    }
  }
  for (int i = 0; i < unit->requires_units.count; ++i) {
    if (start_unit_name(unit->requires_units.items[i], err, err_cap) != 0) { return false; }
  }
  for (int i = 0; i < unit->binds_to.count; ++i) {
    if (start_unit_name(unit->binds_to.items[i], err, err_cap) != 0) { return false; }
  }
  for (int i = 0; i < unit->wants.count; ++i) {
    (void)start_unit_name(unit->wants.items[i], err, err_cap);
  }
  for (int i = 0; i < unit->after.count; ++i) {
    struct unit *after = unit_by_name(unit->after.items[i]);
    if (after != NULL && after->state == STATE_INACTIVE) { (void)start_unit_name(after->name, err, err_cap); }
  }
  return true;
}

void arm_timer(struct unit *unit) {
  time_t now = time(NULL);
  if (unit->on_boot_sec > 0 && unit->next_fire == 0) {
    unit->next_fire = now + unit->on_boot_sec;
  } else if (unit->on_unit_active_sec > 0) {
    unit->next_fire = now + unit->on_unit_active_sec;
  } else if (unit->on_calendar_minute >= 0) {
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_sec = 0;
    tm.tm_min = unit->on_calendar_minute;
    time_t candidate = mktime(&tm);
    if (candidate <= now) { candidate += 3600; }
    unit->next_fire = candidate;
  }
}

int spawn_service(struct unit *unit) {
  if (unit->exec_start[0] == '\0') { return -1; }
  if (!rate_limit_ok(unit)) { return -1; }

  int journal_pipe[2] = {-1, -1};
  bool journal_out = streq(unit->standard_output, "journal") || streq(unit->standard_error, "journal");
  if (journal_out && pipe(journal_pipe) != 0) { return -1; }

  pid_t pid = fork();
  if (pid < 0) {
    if (journal_pipe[0] >= 0) {
      close(journal_pipe[0]);
      close(journal_pipe[1]);
    }
    return -1;
  }
  if (pid == 0) {
    if (journal_pipe[1] >= 0) {
      if (streq(unit->standard_output, "journal")) { dup2(journal_pipe[1], STDOUT_FILENO); }
      if (streq(unit->standard_error, "journal")) { dup2(journal_pipe[1], STDERR_FILENO); }
      close(journal_pipe[0]);
      close(journal_pipe[1]);
    }
    redirect_output(unit->standard_output, STDOUT_FILENO, STDOUT_FILENO);
    redirect_output(unit->standard_error, STDERR_FILENO, STDERR_FILENO);

    if (unit->capability[0] != '\0') {
      if (syscall(SYS_SPORE_APPLY_POLICY, unit->capability) != 0) { _exit(126); }
    }
    if (unit->cpu_budget_ticks != 0) { (void)syscall(SYS_SPORE_SET_BUDGET, 0, unit->cpu_budget_ticks); }
    if (unit->memory_pages != 0 && unit->capability[0] == '\0') {
      char mem_cap[64];
      snprintf(mem_cap, sizeof(mem_cap), "mem:%llu", (unsigned long long)unit->memory_pages);
      (void)syscall(SYS_SPORE_APPLY_POLICY, mem_cap);
    }
    if (unit->user[0] != '\0') {
      struct user_entry user;
      if (user_by_name(unit->user, &user)) {
        (void)setgid(user.gid);
        (void)setuid(user.uid);
        (void)chdir(user.home);
        setenv("HOME", user.home, 1);
        setenv("USER", user.name, 1);
        setenv("LOGNAME", user.name, 1);
        setenv("SHELL", user.shell, 1);
      }
    }
    close_inherited_manager_fds();
    char copy[256];
    copy_text(copy, sizeof(copy), unit->exec_start);
    char *argv[ARG_CAP];
    split_args(copy, argv, ARG_CAP);
    execvp(argv[0], argv);
    perror(argv[0]);
    _exit(127);
  }

  if (journal_pipe[1] >= 0) { close(journal_pipe[1]); }
  unit->journal_fd = journal_pipe[0];
  unit->pid = pid;
  unit->status = 0;
  unit->state = unit->service_type == SERVICE_NOTIFY ? STATE_ACTIVATING : STATE_ACTIVE;
  unit->active_since = time(NULL);
  unit->last_watchdog = unit->active_since;
  copy_text(unit->fail_reason, sizeof(unit->fail_reason), "");
  journal_append(unit, "started pid=%d", (int)pid);
  return 0;
}

int start_unit_name(const char *name, char *err, size_t err_cap) {
  struct unit *unit = unit_by_name(name);
  if (unit == NULL) {
    snprintf(err, err_cap, "%s: not found\n", name);
    return -1;
  }
  if (defer_console_start && streq(unit->name, "console.service")) { return 0; }
  for (int i = 0; i < start_depth; ++i) {
    if (streq(start_stack[i], name)) {
      snprintf(err, err_cap, "dependency cycle at %s\n", name);
      unit->state = STATE_FAILED;
      copy_text(unit->fail_reason, sizeof(unit->fail_reason), "dependency-cycle");
      journal_append(unit, "dependency cycle detected");
      return -1;
    }
  }
  if (unit->state == STATE_ACTIVE || unit->state == STATE_ACTIVATING) { return 0; }
  int result = 0;
  if (start_depth < UNIT_CAP) { start_stack[start_depth++] = unit->name; }
  if (!start_deps(unit, err, err_cap)) {
    result = -1;
    goto out;
  }
  if (unit->type == UNIT_TARGET) {
    unit->state = STATE_ACTIVE;
    unit->active_since = time(NULL);
    announce_unit_started(unit);
    goto out;
  }
  if (unit->type == UNIT_TIMER) {
    unit->state = STATE_ACTIVE;
    unit->active_since = time(NULL);
    if (unit->unit_to_activate[0] == '\0') {
      char base[64];
      copy_text(base, sizeof(base), unit->name);
      char *dot = strrchr(base, '.');
      if (dot != NULL) { strcpy(dot, ".service"); }
      copy_text(unit->unit_to_activate, sizeof(unit->unit_to_activate), base);
    }
    arm_timer(unit);
    announce_unit_started(unit);
    goto out;
  }
  if (unit->type != UNIT_SERVICE) {
    unit->state = STATE_ACTIVE;
    announce_unit_started(unit);
    goto out;
  }

  if (unit->exec_start_pre[0] != '\0' && run_oneshot_command(unit, unit->exec_start_pre) != 0) {
    unit->state = STATE_FAILED;
    copy_text(unit->fail_reason, sizeof(unit->fail_reason), "start-pre-failed");
    result = -1;
    goto out;
  }
  if (unit->service_type == SERVICE_ONESHOT) {
    int rc = run_oneshot_command(unit, unit->exec_start);
    unit->status = rc << 8;
    unit->state = rc == 0 ? STATE_ACTIVE : STATE_FAILED;
    unit->active_since = time(NULL);
    if (rc == 0) { announce_unit_started(unit); }
    if (unit->exec_start_post[0] != '\0') { (void)run_oneshot_command(unit, unit->exec_start_post); }
    result = rc == 0 ? 0 : -1;
    goto out;
  }
  result = spawn_service(unit);
  if (result == 0) { announce_unit_started(unit); }
out:
  if (start_depth > 0) { --start_depth; }
  return result;
}

int run_oneshot_command(struct unit *unit, const char *cmdline) {
  if (cmdline == NULL || cmdline[0] == '\0') { return 0; }
  char copy[256];
  copy_text(copy, sizeof(copy), cmdline);
  char *argv[ARG_CAP];
  split_args(copy, argv, ARG_CAP);
  pid_t pid = fork();
  if (pid < 0) { return 1; }
  if (pid == 0) {
    if (unit->capability[0] != '\0') { (void)syscall(SYS_SPORE_APPLY_POLICY, unit->capability); }
    close_inherited_manager_fds();
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  (void)waitpid(pid, &status, 0);
  int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  journal_append(unit, "oneshot '%s' exit=%d", cmdline, rc);
  return rc;
}

void stop_unit(struct unit *unit) {
  if (unit == NULL || unit->state == STATE_INACTIVE) { return; }
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    if (!units[i].used || &units[i] == unit) { continue; }
    if (list_has(&units[i].part_of, unit->name) || list_has(&units[i].binds_to, unit->name)) { stop_unit(&units[i]); }
  }
  unit->state = STATE_DEACTIVATING;
  if (unit->exec_stop[0] != '\0') { (void)run_oneshot_command(unit, unit->exec_stop); }
  if (unit->pid > 0) {
    kill(unit->pid, SIGTERM);
    time_t deadline = time(NULL) + (unit->timeout_stop_sec > 0 ? unit->timeout_stop_sec : 1);
    int status = 0;
    while (time(NULL) <= deadline) {
      pid_t got = waitpid(unit->pid, &status, WNOHANG);
      if (got == unit->pid) {
        unit->pid = 0;
        break;
      }
    }
    if (unit->pid > 0) {
      kill(unit->pid, SIGKILL);
      (void)waitpid(unit->pid, &status, 0);
      unit->pid = 0;
    }
  }
  if (unit->journal_fd >= 0) {
    close(unit->journal_fd);
    unit->journal_fd = -1;
  }
  unit->state = STATE_INACTIVE;
  unit->inactive_since = time(NULL);
  journal_append(unit, "stopped");
}

bool should_restart(struct unit *unit, int status) {
  bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  bool abnormal = WIFSIGNALED(status);
  switch (unit->restart) {
  case RESTART_NO:
    return false;
  case RESTART_ON_SUCCESS:
    return success;
  case RESTART_ON_FAILURE:
    return !success;
  case RESTART_ON_ABNORMAL:
    return abnormal;
  case RESTART_ON_WATCHDOG:
    return streq(unit->fail_reason, "watchdog");
  case RESTART_ALWAYS:
    return true;
  }
  return false;
}

void reap_children(void) {
  int status = 0;
  for (;;) {
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid <= 0) { return; }
    struct unit *unit = unit_for_pid(pid);
    if (unit == NULL) { continue; }
    unit->pid = 0;
    unit->status = status;
    if (unit->journal_fd >= 0) {
      close(unit->journal_fd);
      unit->journal_fd = -1;
    }
    if (WIFEXITED(status)) {
      journal_append(unit, "exited status=%d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
      journal_append(unit, "signaled signal=%d", WTERMSIG(status));
    }
    bool restart = !shutting_down && should_restart(unit, status);
    if (restart) {
      if (start_limit_exhausted(unit)) {
        unit->state = STATE_FAILED;
        copy_text(unit->fail_reason, sizeof(unit->fail_reason), "start-limit-hit");
        journal_append(unit, "start limit hit");
        continue;
      }
      unit->state = STATE_INACTIVE;
      if (!unit->restart_immediately && unit->restart_sec > 0) { sleep((unsigned)unit->restart_sec); }
      char err[128] = {0};
      if (start_unit_name(unit->name, err, sizeof(err)) != 0) { unit->state = STATE_FAILED; }
    } else {
      unit->state = WIFEXITED(status) && WEXITSTATUS(status) == 0 ? STATE_INACTIVE : STATE_FAILED;
      if (unit->state == STATE_FAILED && unit->fail_reason[0] == '\0') {
        copy_text(unit->fail_reason, sizeof(unit->fail_reason), "exit-failure");
      }
    }
  }
}

void read_journal_fd(struct unit *unit) {
  char buf[192];
  ssize_t n = read(unit->journal_fd, buf, sizeof(buf) - 1);
  if (n <= 0) {
    close(unit->journal_fd);
    unit->journal_fd = -1;
    return;
  }
  buf[n] = '\0';
  char *line = strtok(buf, "\n");
  while (line != NULL) {
    if (strstr(line, "READY=1") != NULL && unit->state == STATE_ACTIVATING) { unit->state = STATE_ACTIVE; }
    if (strstr(line, "WATCHDOG=1") != NULL) { unit->last_watchdog = time(NULL); }
    journal_append(unit, "%s", line);
    line = strtok(NULL, "\n");
  }
}

void check_timers(void) {
  time_t now = time(NULL);
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    struct unit *unit = &units[i];
    if (!unit->used || unit->type != UNIT_TIMER || unit->state != STATE_ACTIVE || unit->next_fire == 0 ||
        unit->next_fire > now) {
      continue;
    }
    char err[128] = {0};
    (void)start_unit_name(unit->unit_to_activate, err, sizeof(err));
    arm_timer(unit);
  }
}

void check_watchdogs(void) {
  time_t now = time(NULL);
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    struct unit *unit = &units[i];
    if (!unit->used || unit->watchdog_sec <= 0 || unit->state != STATE_ACTIVE || unit->pid <= 0) { continue; }
    if (now - unit->last_watchdog > unit->watchdog_sec) {
      copy_text(unit->fail_reason, sizeof(unit->fail_reason), "watchdog");
      journal_append(unit, "watchdog missed");
      kill(unit->pid, SIGKILL);
    }
  }
}

int next_poll_timeout_ms(void) {
  time_t now = time(NULL);
  int best = 1000;
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    struct unit *unit = &units[i];
    if (!unit->used || unit->type != UNIT_TIMER || unit->state != STATE_ACTIVE || unit->next_fire == 0) { continue; }
    int ms = unit->next_fire <= now ? 0 : (int)((unit->next_fire - now) * 1000);
    if (ms < best) { best = ms; }
  }
  return best;
}
