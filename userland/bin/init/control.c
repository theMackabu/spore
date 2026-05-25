#include "mycelium.h"

void write_unit_status(char *out, size_t cap, struct unit *unit) {
  if (unit == NULL) {
    append_response(out, cap, "not-found\n");
    return;
  }
  if (unit->state == STATE_FAILED) {
    append_response(out, cap, "%s: failed (%s)%s%s\n", unit->name, unit->fail_reason[0] ? unit->fail_reason : "failed",
                    unit->pid > 0 ? "; pid " : "", unit->pid > 0 ? "running" : "");
  } else {
    append_response(out, cap, "%s: %s (%s)%s%d%s%s\n", unit->name, state_name(unit->state),
                    unit->pid > 0 ? "running" : type_name(unit->type), unit->pid > 0 ? " since boot; pid " : "",
                    (int)unit->pid, unit->capability[0] ? "; cap=" : "", unit->capability);
  }
}

void list_units(char *out, size_t cap) {
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    if (!units[i].used) { continue; }
    append_response(out, cap, "%-22s %-8s %s\n", units[i].name, state_name(units[i].state),
                    units[i].pid > 0 ? "running" : type_name(units[i].type));
  }
}

void list_timers(char *out, size_t cap) {
  time_t now = time(NULL);
  for (size_t i = 0; i < UNIT_CAP; ++i) {
    struct unit *unit = &units[i];
    if (!unit->used || unit->type != UNIT_TIMER) { continue; }
    if (unit->next_fire > now) {
      append_response(out, cap, "%-16s +%lds     %s\n", unit->name, (long)(unit->next_fire - now),
                      unit->unit_to_activate);
    } else {
      append_response(out, cap, "%-16s waiting   %s\n", unit->name, unit->unit_to_activate);
    }
  }
}

void list_dependencies(char *out, size_t cap, struct unit *unit, int depth) {
  if (unit == NULL || depth > 8) { return; }
  for (int i = 0; i < depth; ++i) {
    append_response(out, cap, "  ");
  }
  append_response(out, cap, "%s\n", unit->name);
  for (int i = 0; i < unit->requires_units.count; ++i) {
    list_dependencies(out, cap, unit_by_name(unit->requires_units.items[i]), depth + 1);
  }
  for (int i = 0; i < unit->wants.count; ++i) {
    list_dependencies(out, cap, unit_by_name(unit->wants.items[i]), depth + 1);
  }
}

void cat_unit(char *out, size_t cap, const char *name) {
  char path[160];
  snprintf(path, sizeof(path), "/etc/mycelium/%s", name);
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    append_response(out, cap, "%s: not found\n", name);
    return;
  }
  char line[256];
  while (fgets(line, sizeof(line), f) != NULL) {
    append_response(out, cap, "%s", line);
  }
  fclose(f);
}

void stop_all_reverse(char *out, size_t cap) {
  shutting_down = true;
  append_response(out, cap, "mycelium: stopping units in reverse order...");
  for (int i = UNIT_CAP - 1; i >= 0; --i) {
    if (units[i].used && units[i].state == STATE_ACTIVE && !streq(units[i].name, "shutdown.target")) {
      append_response(out, cap, " %s", units[i].name);
      stop_unit(&units[i]);
      append_response(out, cap, " stopped...");
    }
  }
  append_response(out, cap, " powering off\n");
}

void handle_command(const char *cmdline, char *out, size_t cap) {
  char copy[256];
  copy_text(copy, sizeof(copy), cmdline);
  char *argv[ARG_CAP];
  int argc = split_args(copy, argv, ARG_CAP);
  if (argc == 0) {
    append_response(out, cap, "empty command\n");
    return;
  }
  if (streq(argv[0], "status")) {
    if (argc == 1) {
      int count = 0;
      int failed = 0;
      for (size_t i = 0; i < UNIT_CAP; ++i) {
        if (units[i].used) {
          ++count;
          if (units[i].state == STATE_FAILED) { ++failed; }
        }
      }
      append_response(out, cap, "mycelium: running, %d units, %d failed\n", count, failed);
    } else {
      write_unit_status(out, cap, unit_by_name(argv[1]));
    }
  } else if (streq(argv[0], "list-units")) {
    list_units(out, cap);
  } else if (streq(argv[0], "list-timers")) {
    list_timers(out, cap);
  } else if (streq(argv[0], "list-dependencies")) {
    list_dependencies(out, cap, unit_by_name(argc > 1 ? argv[1] : "multi-user.target"), 0);
  } else if (streq(argv[0], "start")) {
    if (argc < 2) {
      append_response(out, cap, "start: missing unit\n");
    } else {
      char err[160] = {0};
      if (start_unit_name(argv[1], err, sizeof(err)) == 0) {
        append_response(out, cap, "%s started\n", argv[1]);
      } else {
        append_response(out, cap, "%s", err[0] ? err : "start failed\n");
      }
    }
  } else if (streq(argv[0], "stop")) {
    stop_unit(unit_by_name(argc > 1 ? argv[1] : ""));
    append_response(out, cap, "%s stopped\n", argc > 1 ? argv[1] : "");
  } else if (streq(argv[0], "restart")) {
    if (argc > 1) {
      stop_unit(unit_by_name(argv[1]));
      char err[160] = {0};
      (void)start_unit_name(argv[1], err, sizeof(err));
      append_response(out, cap, "%s restarted\n", argv[1]);
    }
  } else if (streq(argv[0], "reload")) {
    struct unit *unit = unit_by_name(argc > 1 ? argv[1] : "");
    if (unit != NULL && unit->exec_reload[0] != '\0') { (void)run_oneshot_command(unit, unit->exec_reload); }
    append_response(out, cap, "%s reloaded\n", argc > 1 ? argv[1] : "");
  } else if (streq(argv[0], "daemon-reload")) {
    load_units();
    append_response(out, cap, "daemon reloaded\n");
  } else if (streq(argv[0], "logs")) {
    int arg = 1;
    if (argc > 1 && streq(argv[1], "-f")) { arg = 2; }
    struct unit *unit = unit_by_name(arg < argc ? argv[arg] : "");
    if (unit == NULL) {
      append_response(out, cap, "logs: unit not found\n");
    } else {
      append_response(out, cap, "%s", unit->log_mem);
    }
  } else if (streq(argv[0], "show")) {
    struct unit *unit = unit_by_name(argc > 1 ? argv[1] : "");
    if (unit != NULL) {
      append_response(out, cap, "Name=%s\nType=%s\nState=%s\nPID=%d\nCapability=%s\n", unit->name,
                      type_name(unit->type), state_name(unit->state), (int)unit->pid, unit->capability);
    }
  } else if (streq(argv[0], "cat")) {
    cat_unit(out, cap, argc > 1 ? argv[1] : "");
  } else if (streq(argv[0], "is-active")) {
    struct unit *unit = unit_by_name(argc > 1 ? argv[1] : "");
    append_response(out, cap, "%s\n", unit != NULL && unit->state == STATE_ACTIVE ? "active" : "inactive");
  } else if (streq(argv[0], "is-failed")) {
    struct unit *unit = unit_by_name(argc > 1 ? argv[1] : "");
    append_response(out, cap, "%s\n", unit != NULL && unit->state == STATE_FAILED ? "failed" : "ok");
  } else if (streq(argv[0], "enable") || streq(argv[0], "disable")) {
    append_response(out, cap, "%s %s: install links recorded by image manifest\n", argv[0], argc > 1 ? argv[1] : "");
  } else if (streq(argv[0], "isolate")) {
    char err[160] = {0};
    (void)start_unit_name(argc > 1 ? argv[1] : "multi-user.target", err, sizeof(err));
    append_response(out, cap, "isolated %s\n", argc > 1 ? argv[1] : "multi-user.target");
  } else if (streq(argv[0], "poweroff") || streq(argv[0], "reboot")) {
    stop_all_reverse(out, cap);
    write(STDOUT_FILENO, out, strlen(out));
    (void)syscall(SYS_SPORE_SHUTDOWN);
  } else {
    append_response(out, cap, "%s: unknown command\n", argv[0]);
  }
}

void handle_control_client(void) {
  int fd = accept(control_fd, NULL, NULL);
  if (fd < 0) { return; }
  char cmd[256];
  size_t len = 0;
  while (len + 1 < sizeof(cmd)) {
    char c;
    ssize_t n = read(fd, &c, 1);
    if (n <= 0 || c == '\n') { break; }
    cmd[len++] = c;
  }
  cmd[len] = '\0';
  char out[4096] = {0};
  reap_children();
  handle_command(cmd, out, sizeof(out));
  if (out[0] != '\0') { (void)write(fd, out, strlen(out)); }
  close(fd);
}

void setup_control_socket(void) {
  ensure_dir("/run/mycelium");
  (void)unlink("/run/mycelium.sock");
  control_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (control_fd < 0) {
    perror("mycelium: socket");
    return;
  }
  struct sockaddr_un sa;
  memset(&sa, 0, sizeof(sa));
  sa.sun_family = AF_UNIX;
  snprintf(sa.sun_path, sizeof(sa.sun_path), "/run/mycelium.sock");
  if (bind(control_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(control_fd, 8) != 0) {
    perror("mycelium: control");
    close(control_fd);
    control_fd = -1;
  }
}
