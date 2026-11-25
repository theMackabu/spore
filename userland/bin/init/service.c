#include "mycelium.h"

#include <grp.h>
#include <pwd.h>

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
  for (int fd = 3; fd < 64; ++fd) {
    close(fd);
  }
}

bool rate_limit_ok(struct unit *unit) {
  time_t now = time(NULL);
  int kept = 0;
  for (int i = 0; i < unit->start_count && i < START_LIMIT_HISTORY; ++i) {
    if (now - unit->starts[i] <= unit->config.start_limit_interval) { unit->starts[kept++] = unit->starts[i]; }
  }
  unit->start_count = kept;
  if (unit->config.start_limit_burst > 0 && kept >= unit->config.start_limit_burst) {
    copy_text(unit->fail_reason, sizeof(unit->fail_reason), "start-limit-hit");
    unit->state = STATE_FAILED;
    journal_append(unit, "start limit hit");
    return false;
  }
  if (unit->start_count < START_LIMIT_HISTORY) { unit->starts[unit->start_count++] = now; }
  return true;
}

bool start_limit_exhausted(struct unit *unit) {
  if (unit->config.start_limit_burst <= 0) { return false; }
  time_t now = time(NULL);
  int kept = 0;
  for (int i = 0; i < unit->start_count && i < START_LIMIT_HISTORY; ++i) {
    if (now - unit->starts[i] <= unit->config.start_limit_interval) { unit->starts[kept++] = unit->starts[i]; }
  }
  unit->start_count = kept;
  return kept >= unit->config.start_limit_burst;
}

int start_unit_name(const char *name, char *err, size_t err_cap);

static bool start_deps(struct unit *unit, char *err, size_t err_cap) {
  for (int i = 0; i < unit->config.conflicts.count; ++i) {
    struct unit *conflict = unit_by_name(unit->config.conflicts.items[i]);
    if (conflict != NULL && (conflict->state == STATE_ACTIVE || conflict->state == STATE_ACTIVATING)) {
      snprintf(err, err_cap, "%s conflicts with active %s\n", unit->name, conflict->name);
      return false;
    }
  }
  for (int i = 0; i < unit->config.requisite.count; ++i) {
    struct unit *requisite = unit_by_name(unit->config.requisite.items[i]);
    if (requisite == NULL || (requisite->state != STATE_ACTIVE && requisite->state != STATE_ACTIVATING)) {
      snprintf(err, err_cap, "%s requisite %s is not active\n", unit->name, unit->config.requisite.items[i]);
      return false;
    }
  }
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    if (units[i].used && list_has(&units[i].config.before, unit->name) &&
        (units[i].state == STATE_INACTIVE || units[i].state == STATE_FAILED)) {
      (void)start_unit_name(units[i].name, err, err_cap);
    }
  }
  for (int i = 0; i < unit->config.requires_units.count; ++i) {
    if (start_unit_name(unit->config.requires_units.items[i], err, err_cap) != 0) { return false; }
  }
  for (int i = 0; i < unit->config.binds_to.count; ++i) {
    if (start_unit_name(unit->config.binds_to.items[i], err, err_cap) != 0) { return false; }
  }
  for (int i = 0; i < unit->config.wants.count; ++i) {
    (void)start_unit_name(unit->config.wants.items[i], err, err_cap);
  }
  for (int i = 0; i < unit->config.after.count; ++i) {
    struct unit *after = unit_by_name(unit->config.after.items[i]);
    if (after != NULL && after->state == STATE_INACTIVE) { (void)start_unit_name(after->name, err, err_cap); }
  }
  return true;
}

void arm_timer(struct unit *unit) {
  time_t now = time(NULL);
  if (unit->config.on_boot_sec > 0 && unit->next_fire == 0) {
    unit->next_fire = now + unit->config.on_boot_sec;
  } else if (unit->config.on_unit_active_sec > 0) {
    unit->next_fire = now + unit->config.on_unit_active_sec;
  } else if (unit->config.on_calendar_minute >= 0) {
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_sec = 0;
    tm.tm_min = unit->config.on_calendar_minute;
    time_t candidate = mktime(&tm);
    if (candidate <= now) { candidate += 3600; }
    unit->next_fire = candidate;
  }
}

int spawn_service(struct unit *unit) {
  if (unit->config.exec_start[0] == '\0') { return -1; }
  if (!rate_limit_ok(unit)) { return -1; }

  pid_t pid = fork_unit(unit, &unit->journal);
  if (pid < 0) { return -1; }
  if (!pid) {
    struct passwd *user = NULL;
    const char *name = console_user[0] && streq(unit->name, "console.service") ? console_user : unit->config.user;
    if (*name) {
      user = getpwnam(name);
      if (user == NULL || initgroups(user->pw_name, user->pw_gid) != 0) { _exit(126); }
    }
    if (unit->config.capability[0] != '\0') {
      if (syscall(SYS_SPORE_APPLY_POLICY, unit->config.capability) != 0) { _exit(126); }
    }
    if (unit->config.cpu_budget_ticks != 0) { (void)syscall(SYS_SPORE_SET_BUDGET, 0, unit->config.cpu_budget_ticks); }
    if (unit->config.memory_pages != 0 && unit->config.capability[0] == '\0') {
      char mem_cap[64];
      snprintf(mem_cap, sizeof(mem_cap), "mem:%llu", (unsigned long long)unit->config.memory_pages);
      (void)syscall(SYS_SPORE_APPLY_POLICY, mem_cap);
    }
    if (user != NULL) {
      if (setgid(user->pw_gid) != 0 || setuid(user->pw_uid) != 0) { _exit(126); }
      (void)chdir(user->pw_dir);
      setenv("HOME", user->pw_dir, 1);
      setenv("USER", user->pw_name, 1);
      setenv("LOGNAME", user->pw_name, 1);
      setenv("SHELL", user->pw_shell, 1);
    }
    close_inherited_manager_fds();
    char copy[256];
    copy_text(copy, sizeof(copy), unit->config.exec_start);
    char *argv[ARG_CAP];
    split_args(copy, argv, ARG_CAP);
    execvp(argv[0], argv);
    perror(argv[0]);
    _exit(127);
  }

  unit->pid = pid;
  unit->status = 0;
  unit->state = unit->config.service_type == SERVICE_NOTIFY ? STATE_ACTIVATING : STATE_ACTIVE;
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
  if (unit->config.type == UNIT_TARGET) {
    journal_append(unit, "reached target");
    unit->state = STATE_ACTIVE;
    unit->active_since = time(NULL);
    announce_unit_started(unit);
    goto out;
  }
  if (unit->config.type == UNIT_TIMER) {
    unit->state = STATE_ACTIVE;
    unit->active_since = time(NULL);
    if (unit->config.unit_to_activate[0] == '\0') {
      char base[64];
      copy_text(base, sizeof(base), unit->name);
      char *dot = strrchr(base, '.');
      if (dot != NULL) { strcpy(dot, ".service"); }
      copy_text(unit->config.unit_to_activate, sizeof(unit->config.unit_to_activate), base);
    }
    arm_timer(unit);
    announce_unit_started(unit);
    goto out;
  }
  if (unit->config.type != UNIT_SERVICE) {
    unit->state = STATE_ACTIVE;
    announce_unit_started(unit);
    goto out;
  }

  if (unit->config.exec_start_pre[0] != '\0' && run_oneshot_command(unit, unit->config.exec_start_pre) != 0) {
    unit->state = STATE_FAILED;
    copy_text(unit->fail_reason, sizeof(unit->fail_reason), "start-pre-failed");
    result = -1;
    goto out;
  }
  if (unit->config.service_type == SERVICE_ONESHOT) {
    int rc = run_oneshot_command(unit, unit->config.exec_start);
    unit->status = rc << 8;
    unit->state = rc == 0 ? STATE_ACTIVE : STATE_FAILED;
    unit->active_since = time(NULL);
    if (rc == 0) { announce_unit_started(unit); }
    if (unit->config.exec_start_post[0] != '\0') { (void)run_oneshot_command(unit, unit->config.exec_start_post); }
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
  struct journal_stream stream = {.fd = -1};
  pid_t pid = fork_unit(unit, &stream);
  if (pid < 0) {
    journal_append(unit, "fork: %s", strerror(errno));
    return 1;
  }
  if (pid == 0) {
    if (unit->config.capability[0] != '\0') { (void)syscall(SYS_SPORE_APPLY_POLICY, unit->config.capability); }
    close_inherited_manager_fds();
    execvp(argv[0], argv);
    perror(argv[0]);
    _exit(127);
  }
  int status;
  pid_t got;
  for (;;) {
    got = waitpid(pid, &status, stream.fd >= 0 ? WNOHANG : 0);
    if (got == pid || (got < 0 && errno != EINTR)) { break; }
    if (got < 0) { continue; }
    struct pollfd watch = {.fd = stream.fd, .events = POLLIN};
    if (poll(&watch, 1, 50) > 0) { journal_read(unit, &stream); }
  }
  journal_finish(unit, &stream);
  int rc = got != pid ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  journal_append(unit, "oneshot '%s' exit=%d", cmdline, rc);
  return rc;
}

static int64_t monotonic_ms(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return -1; }
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void stop_process(struct unit *unit) {
  pid_t pid = unit->pid;
  kill(pid, streq(unit->name, "console.service") ? SIGHUP : SIGTERM);
  int timeout = unit->config.timeout_stop_sec > 0 ? unit->config.timeout_stop_sec : 1;
  int64_t start = monotonic_ms();
  for (;;) {
    pid_t got = waitpid(pid, NULL, WNOHANG);
    if (got == pid || (got < 0 && errno == ECHILD)) { break; }
    int64_t now = monotonic_ms();
    int64_t remaining = start + (int64_t)timeout * 1000 - now;
    if (start < 0 || now < 0 || remaining <= 0) {
      kill(pid, SIGKILL);
      while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
      break;
    }
    struct pollfd watch = {.fd = unit->journal.fd, .events = POLLIN};
    if (poll(&watch, 1, remaining < 10 ? (int)remaining : 10) > 0) { journal_read(unit, &unit->journal); }
  }
  unit->pid = 0;
}

void stop_unit(struct unit *unit) {
  if (unit == NULL || unit->state == STATE_INACTIVE) { return; }
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    if (!units[i].used || &units[i] == unit) { continue; }
    if (list_has(&units[i].config.part_of, unit->name) || list_has(&units[i].config.binds_to, unit->name)) {
      stop_unit(&units[i]);
    }
  }
  unit->state = STATE_DEACTIVATING;
  if (unit->config.exec_stop[0] != '\0') { (void)run_oneshot_command(unit, unit->config.exec_stop); }
  if (unit->pid > 0) { stop_process(unit); }
  journal_finish(unit, &unit->journal);
  unit->state = STATE_INACTIVE;
  unit->inactive_since = time(NULL);
  journal_append(unit, "stopped");
}

bool should_restart(struct unit *unit, int status) {
  bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  bool abnormal = WIFSIGNALED(status);
  switch (unit->config.restart) {
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
    journal_finish(unit, &unit->journal);
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
      if (!unit->config.restart_immediately && unit->config.restart_sec > 0) {
        sleep((unsigned)unit->config.restart_sec);
      }
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

void check_timers(void) {
  time_t now = time(NULL);
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    struct unit *unit = &units[i];
    if (!unit->used || unit->config.type != UNIT_TIMER || unit->state != STATE_ACTIVE || unit->next_fire == 0 ||
        unit->next_fire > now) {
      continue;
    }
    char err[128] = {0};
    (void)start_unit_name(unit->config.unit_to_activate, err, sizeof(err));
    arm_timer(unit);
  }
}

void check_watchdogs(void) {
  time_t now = time(NULL);
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    struct unit *unit = &units[i];
    if (!unit->used || unit->config.watchdog_sec <= 0 || unit->state != STATE_ACTIVE || unit->pid <= 0) { continue; }
    if (now - unit->last_watchdog > unit->config.watchdog_sec) {
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
    if (!unit->used || unit->config.type != UNIT_TIMER || unit->state != STATE_ACTIVE || unit->next_fire == 0) {
      continue;
    }
    int ms = unit->next_fire <= now ? 0 : (int)((unit->next_fire - now) * 1000);
    if (ms < best) { best = ms; }
  }
  return best;
}
