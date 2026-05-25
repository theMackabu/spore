#include "mycelium.h"

struct unit *unit_by_name(const char *name) {
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    if (units[i].used && streq(units[i].name, name)) { return &units[i]; }
  }
  return NULL;
}

struct unit *unit_for_pid(pid_t pid) {
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    if (units[i].used && units[i].pid == pid) { return &units[i]; }
  }
  return NULL;
}

struct unit *alloc_unit(const char *name) {
  struct unit *existing = unit_by_name(name);
  if (existing != NULL) { return existing; }
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    if (!units[i].used) {
      memset(&units[i], 0, sizeof(units[i]));
      units[i].used = true;
      copy_text(units[i].name, sizeof(units[i].name), name);
      units[i].type = UNIT_SERVICE;
      units[i].service_type = SERVICE_SIMPLE;
      units[i].state = STATE_INACTIVE;
      units[i].restart = RESTART_NO;
      units[i].journal_fd = -1;
      units[i].restart_sec = 1;
      units[i].timeout_stop_sec = 3;
      units[i].start_limit_interval = 10;
      units[i].start_limit_burst = 3;
      units[i].on_calendar_minute = -1;
      copy_text(units[i].standard_output, sizeof(units[i].standard_output), "journal");
      copy_text(units[i].standard_error, sizeof(units[i].standard_error), "journal");
      return &units[i];
    }
  }
  return NULL;
}

void infer_unit_type(struct unit *unit) {
  if (ends_with(unit->name, ".target")) {
    unit->type = UNIT_TARGET;
  } else if (ends_with(unit->name, ".timer")) {
    unit->type = UNIT_TIMER;
  } else if (ends_with(unit->name, ".path")) {
    unit->type = UNIT_PATH;
  } else if (ends_with(unit->name, ".socket")) {
    unit->type = UNIT_SOCKET;
  } else if (ends_with(unit->name, ".mount")) {
    unit->type = UNIT_MOUNT;
  } else {
    unit->type = UNIT_SERVICE;
  }
}

void set_key(struct unit *unit, const char *section, const char *key, const char *value) {
  if (streq(section, "Unit")) {
    if (streq(key, "Description")) {
      copy_text(unit->description, sizeof(unit->description), value);
    } else if (streq(key, "Requires")) {
      list_add_words(&unit->requires_units, value);
    } else if (streq(key, "Wants")) {
      list_add_words(&unit->wants, value);
    } else if (streq(key, "Requisite")) {
      list_add_words(&unit->requisite, value);
    } else if (streq(key, "BindsTo")) {
      list_add_words(&unit->binds_to, value);
    } else if (streq(key, "PartOf")) {
      list_add_words(&unit->part_of, value);
    } else if (streq(key, "Conflicts")) {
      list_add_words(&unit->conflicts, value);
    } else if (streq(key, "After")) {
      list_add_words(&unit->after, value);
    } else if (streq(key, "Before")) {
      list_add_words(&unit->before, value);
    }
  } else if (streq(section, "Service")) {
    if (streq(key, "Type")) {
      if (streq(value, "oneshot")) {
        unit->service_type = SERVICE_ONESHOT;
      } else if (streq(value, "forking")) {
        unit->service_type = SERVICE_FORKING;
      } else if (streq(value, "notify")) {
        unit->service_type = SERVICE_NOTIFY;
      } else if (streq(value, "idle")) {
        unit->service_type = SERVICE_IDLE;
      } else {
        unit->service_type = SERVICE_SIMPLE;
      }
    } else if (streq(key, "ExecStart")) {
      copy_text(unit->exec_start, sizeof(unit->exec_start), value);
    } else if (streq(key, "ExecStartPre")) {
      copy_text(unit->exec_start_pre, sizeof(unit->exec_start_pre), value);
    } else if (streq(key, "ExecStartPost")) {
      copy_text(unit->exec_start_post, sizeof(unit->exec_start_post), value);
    } else if (streq(key, "ExecStop")) {
      copy_text(unit->exec_stop, sizeof(unit->exec_stop), value);
    } else if (streq(key, "ExecReload")) {
      copy_text(unit->exec_reload, sizeof(unit->exec_reload), value);
    } else if (streq(key, "Restart")) {
      if (streq(value, "on-success")) {
        unit->restart = RESTART_ON_SUCCESS;
      } else if (streq(value, "on-failure")) {
        unit->restart = RESTART_ON_FAILURE;
      } else if (streq(value, "on-abnormal")) {
        unit->restart = RESTART_ON_ABNORMAL;
      } else if (streq(value, "on-watchdog")) {
        unit->restart = RESTART_ON_WATCHDOG;
      } else if (streq(value, "always")) {
        unit->restart = RESTART_ALWAYS;
      } else {
        unit->restart = RESTART_NO;
      }
    } else if (streq(key, "RestartSec")) {
      unit->restart_sec = parse_seconds(value);
    } else if (streq(key, "RestartImmediately")) {
      unit->restart_immediately = streq(value, "yes") || streq(value, "true") || streq(value, "1");
    } else if (streq(key, "StartLimitIntervalSec")) {
      unit->start_limit_interval = parse_seconds(value);
    } else if (streq(key, "StartLimitBurst")) {
      unit->start_limit_burst = atoi(value);
    } else if (streq(key, "WatchdogSec")) {
      unit->watchdog_sec = parse_seconds(value);
    } else if (streq(key, "TimeoutStopSec")) {
      unit->timeout_stop_sec = parse_seconds(value);
    } else if (streq(key, "Capability")) {
      copy_text(unit->capability, sizeof(unit->capability), value);
    } else if (streq(key, "CPUBudget")) {
      unit->cpu_budget_ticks = (uint64_t)parse_seconds(value);
    } else if (streq(key, "MemoryPages")) {
      unit->memory_pages = strtoull(value, NULL, 10);
    } else if (streq(key, "User")) {
      copy_text(unit->user, sizeof(unit->user), value);
    } else if (streq(key, "StandardOutput")) {
      copy_text(unit->standard_output, sizeof(unit->standard_output), value);
    } else if (streq(key, "StandardError")) {
      copy_text(unit->standard_error, sizeof(unit->standard_error), value);
    }
  } else if (streq(section, "Timer")) {
    unit->type = UNIT_TIMER;
    if (streq(key, "OnBootSec")) {
      unit->on_boot_sec = parse_seconds(value);
    } else if (streq(key, "OnUnitActiveSec")) {
      unit->on_unit_active_sec = parse_seconds(value);
    } else if (streq(key, "OnUnitInactiveSec")) {
      unit->on_unit_inactive_sec = parse_seconds(value);
    } else if (streq(key, "OnCalendar")) {
      if (strchr(value, ':') != NULL) {
        const char *colon = strchr(value, ':');
        unit->on_calendar_minute = atoi(colon + 1);
      } else {
        unit->on_calendar_minute = atoi(value);
      }
    } else if (streq(key, "Persistent")) {
      unit->persistent = streq(value, "yes") || streq(value, "true");
    } else if (streq(key, "Unit")) {
      copy_text(unit->unit_to_activate, sizeof(unit->unit_to_activate), value);
    }
  } else if (streq(section, "Install")) {
    if (streq(key, "WantedBy") || streq(key, "RequiredBy")) {
      copy_text(unit->wanted_by, sizeof(unit->wanted_by), value);
    }
  }
}

bool parse_unit_file(const char *path, const char *name) {
  FILE *f = fopen(path, "r");
  if (f == NULL) { return false; }
  struct unit *unit = alloc_unit(name);
  if (unit == NULL) {
    fclose(f);
    return false;
  }
  infer_unit_type(unit);
  char section[32] = "Unit";
  char line[512];
  while (fgets(line, sizeof(line), f) != NULL) {
    char *p = trim(line);
    if (*p == '\0' || *p == '#') { continue; }
    if (*p == '[') {
      char *end = strchr(p, ']');
      if (end != NULL) {
        *end = '\0';
        copy_text(section, sizeof(section), p + 1);
      }
      continue;
    }
    char *eq = strchr(p, '=');
    if (eq == NULL) { continue; }
    *eq = '\0';
    set_key(unit, section, trim(p), trim(eq + 1));
  }
  fclose(f);
  return true;
}

void parse_builtin_console(void) {
  struct unit *console = alloc_unit("console.service");
  if (console == NULL) { return; }
  copy_text(console->description, sizeof(console->description), "interactive console shell");
  copy_text(console->exec_start, sizeof(console->exec_start), "/bin/msh");
  copy_text(console->user, sizeof(console->user), "spore");
  copy_text(console->standard_output, sizeof(console->standard_output), "console");
  copy_text(console->standard_error, sizeof(console->standard_error), "console");
  console->restart = RESTART_ALWAYS;
}

void load_units(void) {
  memset(units, 0, sizeof(units));
  parse_builtin_console();
  DIR *dir = opendir("/etc/mycelium");
  if (dir == NULL) { return; }
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_name[0] == '.') { continue; }
    char path[160];
    snprintf(path, sizeof(path), "/etc/mycelium/%s", ent->d_name);
    (void)parse_unit_file(path, ent->d_name);
  }
  closedir(dir);
}

void timestamp(char *out, size_t cap) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm);
}

void journal_append(struct unit *unit, const char *fmt, ...) {
  if (unit == NULL) { return; }
  char msg[LOG_LINE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char ts[32];
  timestamp(ts, sizeof(ts));
  char line[LOG_LINE + 96];
  snprintf(line, sizeof(line), "%s %s: %s\n", ts, unit->name, msg);

  size_t n = strlen(line);
  if (n >= sizeof(unit->log_mem)) {
    n = sizeof(unit->log_mem) - 1;
    line[n] = '\0';
  }
  if (unit->log_len + n >= sizeof(unit->log_mem)) {
    size_t drop = unit->log_len + n - sizeof(unit->log_mem) + 1;
    if (drop < unit->log_len) {
      memmove(unit->log_mem, unit->log_mem + drop, unit->log_len - drop);
      unit->log_len -= drop;
    } else {
      unit->log_len = 0;
    }
  }
  memcpy(unit->log_mem + unit->log_len, line, n);
  unit->log_len += n;
  unit->log_mem[unit->log_len] = '\0';

  ensure_dir("/var/log/mycelium");
  char path[160];
  snprintf(path, sizeof(path), "/var/log/mycelium/%s.log", unit->name);
  FILE *f = fopen(path, "a");
  if (f != NULL) {
    fputs(line, f);
    fclose(f);
  }
}
