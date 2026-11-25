#include "mycelium.h"

extern void mycelium_parse_line(struct unit_config *config, uint8_t *section, const char *line, size_t len);

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
      units[i].config.type = UNIT_SERVICE;
      units[i].config.service_type = SERVICE_SIMPLE;
      units[i].state = STATE_INACTIVE;
      units[i].config.restart = RESTART_NO;
      units[i].journal.fd = -1;
      units[i].config.restart_sec = 1;
      units[i].config.timeout_stop_sec = 3;
      units[i].config.start_limit_interval = 10;
      units[i].config.start_limit_burst = 3;
      units[i].config.on_calendar_minute = -1;
      copy_text(units[i].config.standard_output, sizeof(units[i].config.standard_output), "journal");
      copy_text(units[i].config.standard_error, sizeof(units[i].config.standard_error), "journal");
      return &units[i];
    }
  }
  return NULL;
}

void infer_unit_type(struct unit *unit) {
  if (ends_with(unit->name, ".target")) {
    unit->config.type = UNIT_TARGET;
  } else if (ends_with(unit->name, ".timer")) {
    unit->config.type = UNIT_TIMER;
  } else if (ends_with(unit->name, ".path")) {
    unit->config.type = UNIT_PATH;
  } else if (ends_with(unit->name, ".socket")) {
    unit->config.type = UNIT_SOCKET;
  } else if (ends_with(unit->name, ".mount")) {
    unit->config.type = UNIT_MOUNT;
  } else {
    unit->config.type = UNIT_SERVICE;
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
  uint8_t section = 0;
  char line[512];
  while (fgets(line, sizeof(line), f) != NULL) {
    mycelium_parse_line(&unit->config, &section, line, strlen(line));
  }
  fclose(f);
  return true;
}

void parse_builtin_console(void) {
  struct unit *console = alloc_unit("console.service");
  if (console == NULL) { return; }
  copy_text(console->config.description, sizeof(console->config.description), "interactive console shell");
  copy_text(console->config.exec_start, sizeof(console->config.exec_start), "/bin/bash --login");
  copy_text(console->config.user, sizeof(console->config.user), "spore");
  copy_text(console->config.standard_output, sizeof(console->config.standard_output), "console");
  copy_text(console->config.standard_error, sizeof(console->config.standard_error), "console");
  console->config.restart = RESTART_ALWAYS;
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

  system_log_append(line);
  if (unit->config.type == UNIT_TIMER || strstr(unit->name, "cron") != NULL) { append_log_file("/var/log/cron", line); }

  char path[128];
  snprintf(path, sizeof(path), "/var/log/%s.log", unit->name);
  append_log_file(path, line);
}
