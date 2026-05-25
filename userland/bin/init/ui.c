#include "mycelium.h"

static void read_meminfo(uint64_t *total_kb, uint64_t *free_kb) {
  *total_kb = 0;
  *free_kb = 0;
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL) { return; }
  char key[64];
  unsigned long long value = 0;
  char unit[16];
  while (fscanf(f, "%63s %llu %15s", key, &value, unit) == 3) {
    if (streq(key, "MemTotal:")) { *total_kb = value; }
    if (streq(key, "MemFree:")) { *free_kb = value; }
  }
  fclose(f);
}

static void append_plain_status_log(const char *kind, const char *text) {
  char line[256];
  snprintf(line, sizeof(line), "[%s] %s\n", kind, text);
  boot_log_append(line);
}

static unsigned console_cols(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 40) { return ws.ws_col; }
  return 96;
}

static void print_ok_line(const char *text) {
  unsigned cols = console_cols();
  size_t text_len = strlen(text);
  const size_t marker_len = 6;
  fputs(text, stdout);
  if (cols > text_len + marker_len + 1) {
    for (size_t i = 0; i < cols - text_len - marker_len; ++i) {
      putchar(' ');
    }
  } else {
    putchar(' ');
  }
  fputs("[ " MYC_GREEN "OK" MYC_RESET " ]\n", stdout);
}

static void append_ok_line(char *out, size_t cap, const char *text) {
  unsigned cols = console_cols();
  size_t text_len = strlen(text);
  const size_t marker_len = 6;
  append_response(out, cap, "%s", text);
  if (cols > text_len + marker_len + 1) {
    for (size_t i = 0; i < cols - text_len - marker_len; ++i) {
      append_response(out, cap, " ");
    }
  } else {
    append_response(out, cap, " ");
  }
  append_response(out, cap, "[ " MYC_GREEN "OK" MYC_RESET " ]\n");
}

static void print_spore_logo(void) {
  puts(MYC_CYAN "       .-." MYC_RESET);
  puts(MYC_CYAN "    .-(" MYC_GREEN "   " MYC_CYAN ")-." MYC_RESET);
  puts(MYC_CYAN "   (" MYC_GREEN "   " MYC_BLUE ".-." MYC_GREEN "   " MYC_CYAN ")" MYC_RESET);
  puts(MYC_BLUE "    `-(" MYC_CYAN "   " MYC_BLUE ")-'" MYC_RESET);
  puts(MYC_BLUE "       `-'" MYC_RESET);
  puts(MYC_GREEN "    s p o r e" MYC_RESET);
  putchar('\n');
}

void boot_infof(const char *fmt, ...) {
  char text[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  printf("%s\n", text);
  char line[300];
  snprintf(line, sizeof(line), "%s\n", text);
  boot_log_append(line);
}

void boot_statusf(const char *fmt, ...) {
  char text[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  print_ok_line(text);
  append_plain_status_log("OK", text);
}

void append_done_line(char *out, size_t cap, const char *fmt, ...) {
  char text[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  append_ok_line(out, cap, text);
  append_plain_status_log("OK", text);
}

void boot_banner(void) {
  struct utsname u;
  memset(&u, 0, sizeof(u));
  if (uname(&u) != 0) {
    copy_text(u.sysname, sizeof(u.sysname), "Spore");
    copy_text(u.release, sizeof(u.release), "?");
    copy_text(u.version, sizeof(u.version), "Spore Kernel");
  }

  uint64_t total_kb = 0;
  uint64_t free_kb = 0;
  read_meminfo(&total_kb, &free_kb);

  print_spore_logo();
  boot_infof("Running %s Kernel %s.", u.sysname, u.release);
  if (total_kb != 0) {
    boot_infof("Total Memory available: %llukB, Memory free: %llukB.", (unsigned long long)total_kb,
               (unsigned long long)free_kb);
  }
  boot_infof("Starting INIT (process 1).");
  boot_infof("INIT: mycelium booting.");
}

static void print_file(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) { return; }
  char buf[128];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) { break; }
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }
  close(fd);
}

static void run_motd_script(const char *path) {
  pid_t pid = fork();
  if (pid < 0) { return; }
  if (pid == 0) {
    execl("/bin/msh", "msh", path, (char *)NULL);
    _exit(127);
  }
  int status = 0;
  (void)waitpid(pid, &status, 0);
}

static int motd_name_cmp(const void *a, const void *b) {
  const char *const *lhs = a;
  const char *const *rhs = b;
  return strcmp(*lhs, *rhs);
}

static void run_update_motd(void) {
  DIR *dir = opendir("/etc/update-motd.d");
  if (dir == NULL) { return; }
  char names[24][64];
  char *order[24];
  size_t count = 0;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL && count < 24) {
    if (ent->d_name[0] == '.') { continue; }
    copy_text(names[count], sizeof(names[count]), ent->d_name);
    order[count] = names[count];
    ++count;
  }
  closedir(dir);
  qsort(order, count, sizeof(order[0]), motd_name_cmp);
  for (size_t i = 0; i < count; ++i) {
    char path[160];
    snprintf(path, sizeof(path), "/etc/update-motd.d/%s", order[i]);
    struct stat st;
    if (stat(path, &st) != 0 || (st.st_mode & 0111) == 0) { continue; }
    run_motd_script(path);
  }
}

void boot_run_login_banner(void) {
  print_file("/etc/motd");
  run_update_motd();
}

void announce_unit_started(struct unit *unit) {
  if (!boot_status_enabled || unit == NULL || unit->boot_reported) { return; }
  unit->boot_reported = true;
  if (unit->type == UNIT_TARGET) {
    boot_statusf("Reached target %s.", unit->name);
  } else if (unit->description[0] != '\0') {
    boot_statusf("Started %s.", unit->description);
  } else {
    boot_statusf("Started %s.", unit->name);
  }
}
