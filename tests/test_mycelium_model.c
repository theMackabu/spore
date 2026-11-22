#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
  UNIT_CAP = 8,
  DEP_CAP = 4,
};

struct unit {
  char name[32];
  char requires[DEP_CAP][32];
  int require_count;
  char after[DEP_CAP][32];
  int after_count;
  char exec_start[64];
  char capability[32];
};

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
    ++s;
  }
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
    *--end = '\0';
  }
  return s;
}

static void add_words(char items[DEP_CAP][32], int *count, const char *value) {
  char copy[128];
  snprintf(copy, sizeof(copy), "%s", value);
  char *save = NULL;
  for (char *p = strtok_r(copy, " \t", &save); p != NULL && *count < DEP_CAP; p = strtok_r(NULL, " \t", &save)) {
    snprintf(items[(*count)++], 32, "%s", p);
  }
}

static void parse_unit_text(struct unit *unit, const char *text) {
  char copy[512];
  snprintf(copy, sizeof(copy), "%s", text);
  char section[32] = "Unit";
  char *save = NULL;
  for (char *line = strtok_r(copy, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
    char *p = trim(line);
    if (*p == '\0' || *p == '#') { continue; }
    if (*p == '[') {
      char *end = strchr(p, ']');
      assert(end != NULL);
      *end = '\0';
      snprintf(section, sizeof(section), "%s", p + 1);
      continue;
    }
    char *eq = strchr(p, '=');
    assert(eq != NULL);
    *eq = '\0';
    char *key = trim(p);
    char *value = trim(eq + 1);
    if (strcmp(section, "Unit") == 0 && strcmp(key, "Requires") == 0) {
      add_words(unit->requires, &unit->require_count, value);
    } else if (strcmp(section, "Unit") == 0 && strcmp(key, "After") == 0) {
      add_words(unit->after, &unit->after_count, value);
    } else if (strcmp(section, "Service") == 0 && strcmp(key, "ExecStart") == 0) {
      snprintf(unit->exec_start, sizeof(unit->exec_start), "%s", value);
    } else if (strcmp(section, "Service") == 0 && strcmp(key, "Capability") == 0) {
      snprintf(unit->capability, sizeof(unit->capability), "%s", value);
    }
  }
}

static int find_unit(struct unit *units, int count, const char *name) {
  for (int i = 0; i < count; ++i) {
    if (strcmp(units[i].name, name) == 0) { return i; }
  }
  return -1;
}

static bool visit(struct unit *units, int count, int index, int *mark, char order[UNIT_CAP][32], int *order_count) {
  if (mark[index] == 1) { return false; }
  if (mark[index] == 2) { return true; }
  mark[index] = 1;
  for (int i = 0; i < units[index].require_count; ++i) {
    int dep = find_unit(units, count, units[index].requires[i]);
    if (dep >= 0 && !visit(units, count, dep, mark, order, order_count)) { return false; }
  }
  for (int i = 0; i < units[index].after_count; ++i) {
    int dep = find_unit(units, count, units[index].after[i]);
    if (dep >= 0 && !visit(units, count, dep, mark, order, order_count)) { return false; }
  }
  mark[index] = 2;
  snprintf(order[(*order_count)++], 32, "%s", units[index].name);
  return true;
}

static bool topo(struct unit *units, int count, const char *root, char order[UNIT_CAP][32], int *order_count) {
  int mark[UNIT_CAP] = {0};
  int index = find_unit(units, count, root);
  assert(index >= 0);
  *order_count = 0;
  return visit(units, count, index, mark, order, order_count);
}

static void test_unit_parser(void) {
  struct unit unit = {.name = "demo.service"};
  parse_unit_text(&unit, "[Unit]\n"
                         "Requires=network.target logger.service\n"
                         "After=logger.service\n"
                         "[Service]\n"
                         "ExecStart=/bin/true\n"
                         "Capability=fs:/tmp\n");
  assert(unit.require_count == 2);
  assert(strcmp(unit.requires[0], "network.target") == 0);
  assert(strcmp(unit.after[0], "logger.service") == 0);
  assert(strcmp(unit.exec_start, "/bin/true") == 0);
  assert(strcmp(unit.capability, "fs:/tmp") == 0);
}

static void test_dependency_order_and_cycles(void) {
  struct unit units[UNIT_CAP] = {
    {.name = "multi-user.target", .requires = {"syslog.service", "console.service"}, .require_count = 2},
    {.name = "syslog.service"},
    {.name = "console.service", .after = {"syslog.service"}, .after_count = 1},
  };
  char order[UNIT_CAP][32];
  int count = 0;
  assert(topo(units, 3, "multi-user.target", order, &count));
  assert(count == 3);
  assert(strcmp(order[0], "syslog.service") == 0);
  assert(strcmp(order[1], "console.service") == 0);
  assert(strcmp(order[2], "multi-user.target") == 0);

  struct unit cycle[UNIT_CAP] = {
    {.name = "a.service", .requires = {"b.service"}, .require_count = 1},
    {.name = "b.service", .requires = {"a.service"}, .require_count = 1},
  };
  assert(!topo(cycle, 2, "a.service", order, &count));
}

static void test_calendar_timer_math(void) {
  int now_min = 30;
  int on_calendar = 31;
  int delta = on_calendar > now_min ? on_calendar - now_min : 60 - now_min + on_calendar;
  assert(delta == 1);
  now_min = 45;
  delta = on_calendar > now_min ? on_calendar - now_min : 60 - now_min + on_calendar;
  assert(delta == 46);
}

int main(void) {
  test_unit_parser();
  test_dependency_order_and_cycles();
  test_calendar_timer_math();
  return 0;
}
