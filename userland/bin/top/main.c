#include <fcntl.h>
#include <spore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum {
  MAX_PROCS = 32,
};

static struct termios saved_termios;
static int have_saved_termios;

struct proc_row {
  unsigned pid;
  unsigned ppid;
  char state[16];
  char wait[16];
  unsigned long long vsz_pages;
  unsigned long long rss_pages;
  unsigned long long minflt;
  unsigned long long majflt;
  unsigned long long cpu_ticks;
  unsigned long long age_ticks;
  unsigned long long budget_remaining;
  unsigned long long budget_max;
  unsigned long long unsupported_syscalls;
  unsigned long long last_unsupported_syscall;
  unsigned long long unsupported_ioctls;
  unsigned long long last_unsupported_ioctl;
  char name[32];
  char exec_path[128];
  char cwd[64];
  char cmdline[160];
};

static void restore_terminal(void) {
  static const char leave_alt[] = "\033[?25h\033[?1049l";
  (void)write(STDOUT_FILENO, leave_alt, sizeof(leave_alt) - 1);
  if (have_saved_termios) { (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios); }
}

static void setup_terminal(void) {
  if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
    struct termios raw = saved_termios;
    have_saved_termios = 1;
    raw.c_lflag &= ~(unsigned)(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  }
  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (flags >= 0) { (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK); }
  static const char enter_alt[] = "\033[?1049h\033[?25l\033[2J\033[H";
  (void)write(STDOUT_FILENO, enter_alt, sizeof(enter_alt) - 1);
  atexit(restore_terminal);
}

static unsigned screen_rows(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row >= 8) { return ws.ws_row; }
  return 24;
}

static void sleep_short(void) {
  struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
  (void)nanosleep(&ts, NULL);
}

static int read_key(void) {
  unsigned char c;
  ssize_t n = read(STDIN_FILENO, &c, 1);
  return n == 1 ? c : -1;
}

static int proc_cmp(const void *a, const void *b) {
  const struct proc_row *lhs = a;
  const struct proc_row *rhs = b;
  if (lhs->rss_pages < rhs->rss_pages) { return 1; }
  if (lhs->rss_pages > rhs->rss_pages) { return -1; }
  if (lhs->cpu_ticks < rhs->cpu_ticks) { return 1; }
  if (lhs->cpu_ticks > rhs->cpu_ticks) { return -1; }
  if (lhs->pid > rhs->pid) { return 1; }
  if (lhs->pid < rhs->pid) { return -1; }
  return 0;
}

static void fmt_mem(unsigned long long pages, char *out, size_t cap) {
  unsigned long long kib = pages * 4ull;
  if (kib < 1024) {
    snprintf(out, cap, "%lluK", kib);
    return;
  }
  unsigned long long tenths = (kib * 10ull + 512ull) / 1024ull;
  if (tenths < 100 || (tenths % 10ull) != 0) {
    snprintf(out, cap, "%llu.%lluM", tenths / 10ull, tenths % 10ull);
    return;
  }
  snprintf(out, cap, "%lluM", tenths / 10ull);
}

static void fmt_kib(unsigned long long kib, char *out, size_t cap) {
  if (kib < 1024) {
    snprintf(out, cap, "%lluK", kib);
    return;
  }
  unsigned long long tenths = (kib * 10ull + 512ull) / 1024ull;
  if (tenths < 100 || (tenths % 10ull) != 0) {
    snprintf(out, cap, "%llu.%lluM", tenths / 10ull, tenths % 10ull);
    return;
  }
  snprintf(out, cap, "%lluM", tenths / 10ull);
}

static int read_meminfo(unsigned long long *total_kib, unsigned long long *used_kib, unsigned long long *free_kib) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL) { return -1; }
  char line[128];
  *total_kib = 0;
  *used_kib = 0;
  *free_kib = 0;
  while (fgets(line, sizeof(line), f) != NULL) {
    char key[32];
    unsigned long long value;
    if (sscanf(line, "%31s %llu", key, &value) != 2) { continue; }
    if (streq(key, "MemTotalKiB:")) {
      *total_kib = value;
    } else if (streq(key, "MemUsedKiB:")) {
      *used_kib = value;
    } else if (streq(key, "MemFreeKiB:")) {
      *free_kib = value;
    }
  }
  fclose(f);
  return *total_kib == 0 ? -1 : 0;
}

static long load_procs(struct proc_row *infos, size_t cap) {
  FILE *f = fopen("/proc/procinfo", "r");
  if (f == NULL) { return -1; }
  char header[512];
  (void)fgets(header, sizeof(header), f);
  long n = 0;
  while ((size_t)n < cap &&
         fscanf(f, "%u %u %15s %15s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %31s %127s %63s "
                   "%159[^\n]\n",
                &infos[n].pid, &infos[n].ppid, infos[n].state, infos[n].wait, &infos[n].vsz_pages, &infos[n].rss_pages,
                &infos[n].minflt, &infos[n].majflt, &infos[n].cpu_ticks, &infos[n].age_ticks,
                &infos[n].budget_remaining, &infos[n].budget_max, &infos[n].unsupported_syscalls,
                &infos[n].last_unsupported_syscall, &infos[n].unsupported_ioctls, &infos[n].last_unsupported_ioctl,
                infos[n].name, infos[n].exec_path, infos[n].cwd, infos[n].cmdline) == 20) {
    ++n;
  }
  fclose(f);
  return n;
}

static int draw(void) {
  struct proc_row infos[MAX_PROCS];
  long n = load_procs(infos, MAX_PROCS);
  if (n < 0) { return -1; }
  qsort(infos, (size_t)n, sizeof(infos[0]), proc_cmp);

  unsigned running = 0;
  unsigned sleeping = 0;
  unsigned zombie = 0;
  unsigned long long total_cpu = 0;
  unsigned long long total_age = 0;
  for (long i = 0; i < n; ++i) {
    if (streq(infos[i].state, "running")) {
      ++running;
    } else if (streq(infos[i].state, "zombie")) {
      ++zombie;
    } else {
      ++sleeping;
    }
    total_cpu += infos[i].cpu_ticks;
    total_age += infos[i].age_ticks;
  }
  unsigned long long busy_tenths = total_age == 0 ? 0 : (total_cpu * 1000ull) / total_age;
  if (busy_tenths > 1000) { busy_tenths = 1000; }
  unsigned long long idle_tenths = 1000ull - busy_tenths;
  unsigned long long total_kib = 0;
  unsigned long long used_kib = 0;
  unsigned long long free_kib = 0;
  char used_mem[16] = "?";
  char free_mem[16] = "?";
  if (read_meminfo(&total_kib, &used_kib, &free_kib) == 0) {
    fmt_kib(used_kib, used_mem, sizeof(used_mem));
    fmt_kib(free_kib, free_mem, sizeof(free_mem));
  }
  struct timespec now;
  (void)clock_gettime(CLOCK_REALTIME, &now);
  long long local = (long long)now.tv_sec - 7 * 60 * 60;
  long long day_seconds = local % 86400;
  if (day_seconds < 0) { day_seconds += 86400; }

  unsigned rows = screen_rows();
  printf("\033[H\033[2J");
  printf("Processes: %ld total, %u running, %u sleeping, %u zombie, %ld threads%*s%02lld:%02lld:%02lld\r\n", n, running,
         sleeping, zombie, n, 1, "", day_seconds / 3600, (day_seconds % 3600) / 60, day_seconds % 60);
  printf("Load Avg: 0.00, 0.00, 0.00  CPU usage: %llu.%llu%% user, 0.0%% sys, %llu.%llu%% idle\r\n",
         busy_tenths / 10ull, busy_tenths % 10ull, idle_tenths / 10ull, idle_tenths % 10ull);
  printf("PhysMem: %s used, %s unused.\r\n\r\n", used_mem, free_mem);
  printf("PID  COMMAND         %%CPU TIME     MEM    PPID  STATE     BUDGET\r\n");
  unsigned used_rows = 5;
  for (long i = 0; i < n && used_rows + 1 < rows; ++i, ++used_rows) {
    char budget[32];
    char mem[16];
    char time[24];
    unsigned long long cpu_tenths = infos[i].age_ticks == 0 ? 0 : (infos[i].cpu_ticks * 1000ull) / infos[i].age_ticks;
    if (infos[i].budget_max == 0) {
      snprintf(budget, sizeof(budget), "unlimited");
    } else {
      snprintf(budget, sizeof(budget), "%llu/%llu", infos[i].budget_remaining, infos[i].budget_max);
    }
    fmt_mem(infos[i].rss_pages, mem, sizeof(mem));
    snprintf(time, sizeof(time), "%llu:%02llu.%02llu", (infos[i].cpu_ticks / 100ull) / 60ull,
             (infos[i].cpu_ticks / 100ull) % 60ull, infos[i].cpu_ticks % 100ull);
    printf("%3u  %-14s %3llu.%1llu %-8s %-6s %4u  %-8s  %s\r\n", infos[i].pid, infos[i].name, cpu_tenths / 10ull,
           cpu_tenths % 10ull, time, mem, infos[i].ppid, infos[i].state, budget);
  }
  while (used_rows++ < rows - 1) {
    printf("\033[K\r\n");
  }
  printf("\033[7m ^L refresh   q quit \033[m");
  fflush(stdout);
  return 0;
}

int main(int argc, char **argv) {
  int once = argc > 1 && streq(argv[1], "-b");
  if (!once) { setup_terminal(); }
  for (;;) {
    if (draw() != 0) {
      perror("top");
      return EXIT_FAILURE;
    }
    if (once) { return EXIT_SUCCESS; }
    for (int i = 0; i < 10; ++i) {
      int key = read_key();
      if (key == 'q' || key == 'Q' || key == 3) { return EXIT_SUCCESS; }
      if (key == 12) { break; }
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000};
      (void)nanosleep(&ts, NULL);
    }
    sleep_short();
  }
}
