#include <spore.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int eprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = vfprintf(stderr, fmt, ap);
  va_end(ap);
  return rc;
}

int usage(const char *tool, const char *usage) {
  eprintf("usage: %s %s\n", tool, usage);
  return EXIT_USAGE;
}

const char *basename(const char *path) {
  const char *name = path;
  for (const char *p = path; *p != '\0'; ++p) {
    if (*p == '/' && p[1] != '\0') { name = p + 1; }
  }
  return name;
}

bool streq(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

static void copy_field(char *dst, size_t cap, const char *src) {
  if (cap == 0) { return; }
  size_t n = 0;
  while (n + 1 < cap && src[n] != '\0') {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
}

static bool parse_uint(const char *s, unsigned *out) {
  unsigned v = 0;
  if (*s == '\0') { return false; }
  for (const char *p = s; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') { return false; }
    v = v * 10u + (unsigned)(*p - '0');
  }
  *out = v;
  return true;
}

static bool next_field(char **p, char **field) {
  if (*p == NULL) { return false; }
  *field = *p;
  char *colon = strchr(*p, ':');
  if (colon == NULL) {
    *p = NULL;
  } else {
    *colon = '\0';
    *p = colon + 1;
  }
  return true;
}

static bool parse_passwd_line(char *line, struct user_entry *out) {
  char *nl = strchr(line, '\n');
  if (nl != NULL) { *nl = '\0'; }
  char *p = line;
  char *name = NULL;
  char *password = NULL;
  char *uid_s = NULL;
  char *gid_s = NULL;
  char *gecos = NULL;
  char *home = NULL;
  char *shell = NULL;
  if (!next_field(&p, &name) || !next_field(&p, &password) || !next_field(&p, &uid_s) || !next_field(&p, &gid_s) ||
      !next_field(&p, &gecos) || !next_field(&p, &home) || !next_field(&p, &shell)) {
    return false;
  }
  unsigned uid = 0;
  unsigned gid = 0;
  if (!parse_uint(uid_s, &uid) || !parse_uint(gid_s, &gid)) { return false; }
  copy_field(out->name, sizeof(out->name), name);
  copy_field(out->password, sizeof(out->password), password);
  out->uid = uid;
  out->gid = gid;
  copy_field(out->gecos, sizeof(out->gecos), gecos);
  copy_field(out->home, sizeof(out->home), home);
  copy_field(out->shell, sizeof(out->shell), shell);
  return true;
}

static bool parse_group_line(char *line, struct group_entry *out) {
  char *nl = strchr(line, '\n');
  if (nl != NULL) { *nl = '\0'; }
  char *p = line;
  char *name = NULL;
  char *password = NULL;
  char *gid_s = NULL;
  if (!next_field(&p, &name) || !next_field(&p, &password) || !next_field(&p, &gid_s)) { return false; }
  (void)password;
  unsigned gid = 0;
  if (!parse_uint(gid_s, &gid)) { return false; }
  copy_field(out->name, sizeof(out->name), name);
  out->gid = gid;
  return true;
}

bool user_by_name(const char *name, struct user_entry *out) {
  FILE *f = fopen("/etc/passwd", "r");
  if (f == NULL) { return false; }
  char line[384];
  struct user_entry entry;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_passwd_line(line, &entry) && strcmp(entry.name, name) == 0) {
      if (out != NULL) { *out = entry; }
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

bool user_by_uid(unsigned uid, struct user_entry *out) {
  FILE *f = fopen("/etc/passwd", "r");
  if (f == NULL) { return false; }
  char line[384];
  struct user_entry entry;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_passwd_line(line, &entry) && entry.uid == uid) {
      if (out != NULL) { *out = entry; }
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

bool group_by_name(const char *name, struct group_entry *out) {
  FILE *f = fopen("/etc/group", "r");
  if (f == NULL) { return false; }
  char line[256];
  struct group_entry entry;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_group_line(line, &entry) && strcmp(entry.name, name) == 0) {
      if (out != NULL) { *out = entry; }
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

bool group_by_gid(unsigned gid, struct group_entry *out) {
  FILE *f = fopen("/etc/group", "r");
  if (f == NULL) { return false; }
  char line[256];
  struct group_entry entry;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_group_line(line, &entry) && entry.gid == gid) {
      if (out != NULL) { *out = entry; }
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

unsigned next_user_id(void) {
  FILE *f = fopen("/etc/passwd", "r");
  if (f == NULL) { return 1000; }
  char line[384];
  struct user_entry entry;
  unsigned max = 999;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_passwd_line(line, &entry) && entry.uid > max) { max = entry.uid; }
  }
  fclose(f);
  return max + 1;
}

static uint64_t fnv1a_mix(uint64_t h, const char *s) {
  while (*s != '\0') {
    h ^= (unsigned char)*s++;
    h *= 1099511628211ull;
  }
  return h;
}

static void hash_password(const char *name, const char *password, char *out, size_t cap) {
  uint64_t h = 1469598103934665603ull;
  h = fnv1a_mix(h, "spore-shadow-v1:");
  h = fnv1a_mix(h, name);
  h = fnv1a_mix(h, ":");
  h = fnv1a_mix(h, password);
  h ^= h >> 32;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  snprintf(out, cap, "$spore$%016llx", (unsigned long long)h);
}

static bool shadow_field(const char *name, char *out, size_t cap) {
  FILE *f = fopen("/etc/shadow", "r");
  if (f == NULL) { return false; }
  char line[256];
  size_t name_len = strlen(name);
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, name, name_len) == 0 && line[name_len] == ':') {
      char *p = line + name_len + 1;
      char *end = strchr(p, ':');
      if (end == NULL) { end = strchr(p, '\n'); }
      if (end != NULL) { *end = '\0'; }
      copy_field(out, cap, p);
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

bool password_matches(const char *name, const char *password) {
  char stored[96];
  if (!shadow_field(name, stored, sizeof(stored))) { return false; }
  if (stored[0] == '\0') { return true; }
  if (stored[0] == '!') { return false; }
  char hashed[96];
  hash_password(name, password, hashed, sizeof(hashed));
  return strcmp(stored, hashed) == 0;
}

static bool rewrite_shadow(const char *name, const char *field, bool remove) {
  FILE *in = fopen("/etc/shadow", "r");
  FILE *out = fopen("/etc/shadow.tmp", "w");
  if (out == NULL) {
    if (in != NULL) { fclose(in); }
    return false;
  }
  bool found = false;
  size_t name_len = strlen(name);
  if (in != NULL) {
    char line[256];
    while (fgets(line, sizeof(line), in) != NULL) {
      if (strncmp(line, name, name_len) == 0 && line[name_len] == ':') {
        found = true;
        if (!remove) { fprintf(out, "%s:%s:0:0:99999:7:::\n", name, field); }
      } else {
        fputs(line, out);
      }
    }
    fclose(in);
  }
  if (!found && !remove) { fprintf(out, "%s:%s:0:0:99999:7:::\n", name, field); }
  fclose(out);
  return rename("/etc/shadow.tmp", "/etc/shadow") == 0;
}

bool set_shadow_password(const char *name, const char *password) {
  char hashed[96];
  hash_password(name, password, hashed, sizeof(hashed));
  return rewrite_shadow(name, hashed, false);
}

bool add_shadow_user(const char *name) {
  return rewrite_shadow(name, "", false);
}

bool remove_shadow_user(const char *name) {
  return rewrite_shadow(name, "", true);
}

bool sudo_user_allowed(const char *name, bool *nopasswd) {
  FILE *f = fopen("/etc/sudoers", "r");
  if (f == NULL) { return false; }
  char line[256];
  bool allowed = false;
  bool no_password = false;
  size_t name_len = strlen(name);
  while (fgets(line, sizeof(line), f) != NULL) {
    char *p = line;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (*p == '\0' || *p == '\n' || *p == '#') { continue; }
    if ((strncmp(p, name, name_len) == 0 && (p[name_len] == ' ' || p[name_len] == '\t')) ||
        (strncmp(p, "%sudo", 5) == 0 && strcmp(name, "spore") == 0)) {
      allowed = true;
      if (strstr(p, "NOPASSWD") != NULL) { no_password = true; }
    }
  }
  fclose(f);
  if (nopasswd != NULL) { *nopasswd = no_password; }
  return allowed;
}
