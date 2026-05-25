#include "mycelium.h"

const char *state_name(enum unit_state state) {
  switch (state) {
  case STATE_INACTIVE:
    return "inactive";
  case STATE_ACTIVATING:
    return "activating";
  case STATE_ACTIVE:
    return "active";
  case STATE_DEACTIVATING:
    return "deactivating";
  case STATE_FAILED:
    return "failed";
  }
  return "?";
}

const char *type_name(enum unit_type type) {
  switch (type) {
  case UNIT_SERVICE:
    return "service";
  case UNIT_TARGET:
    return "target";
  case UNIT_TIMER:
    return "timer";
  case UNIT_PATH:
    return "path";
  case UNIT_SOCKET:
    return "socket";
  case UNIT_MOUNT:
    return "mount";
  }
  return "?";
}

void copy_text(char *dst, size_t cap, const char *src) {
  if (cap == 0) { return; }
  snprintf(dst, cap, "%s", src == NULL ? "" : src);
}

char *trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
    ++s;
  }
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
    *--end = '\0';
  }
  return s;
}

bool ends_with(const char *s, const char *suffix) {
  size_t a = strlen(s);
  size_t b = strlen(suffix);
  return a >= b && strcmp(s + a - b, suffix) == 0;
}

void ensure_dir(const char *path) {
  char tmp[160];
  copy_text(tmp, sizeof(tmp), path);
  for (char *p = tmp + 1; *p != '\0'; ++p) {
    if (*p == '/') {
      *p = '\0';
      (void)mkdir(tmp, 0755);
      *p = '/';
    }
  }
  (void)mkdir(tmp, 0755);
}

void append_response(char *out, size_t cap, const char *fmt, ...) {
  size_t len = strlen(out);
  if (len >= cap) { return; }
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(out + len, cap - len, fmt, ap);
  va_end(ap);
}

int parse_seconds(const char *s) {
  char *end = NULL;
  long value = strtol(s, &end, 10);
  if (end != NULL && *end == 's') { ++end; }
  if (end == s || (end != NULL && *end != '\0') || value < 0 || value > 86400) { return 0; }
  return (int)value;
}

void list_add_words(struct string_list *list, const char *value) {
  char copy[256];
  copy_text(copy, sizeof(copy), value);
  for (char *p = strtok(copy, " \t"); p != NULL && list->count < DEP_CAP; p = strtok(NULL, " \t")) {
    copy_text(list->items[list->count++], sizeof(list->items[0]), p);
  }
}

bool list_has(const struct string_list *list, const char *name) {
  for (int i = 0; i < list->count; ++i) {
    if (streq(list->items[i], name)) { return true; }
  }
  return false;
}

int split_args(char *cmd, char **argv, int cap) {
  int argc = 0;
  char *p = cmd;
  while (*p != '\0' && argc + 1 < cap) {
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (*p == '\0') { break; }
    if (*p == '"' || *p == '\'') {
      char quote = *p++;
      argv[argc++] = p;
      while (*p != '\0' && *p != quote) {
        ++p;
      }
    } else {
      argv[argc++] = p;
      while (*p != '\0' && *p != ' ' && *p != '\t') {
        ++p;
      }
    }
    if (*p != '\0') { *p++ = '\0'; }
  }
  argv[argc] = NULL;
  return argc;
}

void load_environment_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (f == NULL) { return; }

  char line[256];
  while (fgets(line, sizeof(line), f) != NULL) {
    char *entry = trim(line);
    if (entry[0] == '\0' || entry[0] == '#') { continue; }

    char *eq = strchr(entry, '=');
    if (eq == NULL || eq == entry) { continue; }
    *eq = '\0';
    char *key = trim(entry);
    char *value = trim(eq + 1);
    size_t value_len = strlen(value);
    if (value_len >= 2 &&
        ((value[0] == '\'' && value[value_len - 1] == '\'') || (value[0] == '"' && value[value_len - 1] == '"'))) {
      value[value_len - 1] = '\0';
      ++value;
    }

    bool valid_key = key[0] != '\0';
    for (char *p = key; *p != '\0'; ++p) {
      if (!(isalnum((unsigned char)*p) || *p == '_')) {
        valid_key = false;
        break;
      }
    }
    if (valid_key) { setenv(key, value, 0); }
  }
  fclose(f);
}
