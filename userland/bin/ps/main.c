#include <spore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char state_letter(const char *state) {
  if (streq(state, "running")) { return 'R'; }
  if (streq(state, "blocked")) { return 'S'; }
  if (streq(state, "zombie")) { return 'Z'; }
  return '?';
}

static void fmt_time(unsigned long long ticks, char *out, size_t cap) {
  unsigned long long seconds = ticks / 100ull;
  unsigned long long centis = ticks % 100ull;
  snprintf(out, cap, "%llu:%02llu.%02llu", seconds / 60ull, seconds % 60ull, centis);
}

static void fmt_percent(unsigned long long tenths, bool nonzero, char *out, size_t cap) {
  if (tenths == 0 && nonzero) {
    snprintf(out, cap, "<0.1");
    return;
  }
  snprintf(out, cap, "%llu.%llu", tenths / 10ull, tenths % 10ull);
}

static int read_mem_total(unsigned long long *total_kib) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL) { return -1; }
  char key[32];
  unsigned long long value;
  *total_kib = 0;
  while (fscanf(f, "%31s %llu", key, &value) == 2) {
    if (streq(key, "MemTotalKiB:")) {
      *total_kib = value;
      break;
    }
  }
  fclose(f);
  return *total_kib == 0 ? -1 : 0;
}

static int load_rows(struct proc_row *rows, size_t cap) {
  FILE *f = fopen("/proc/procinfo", "r");
  if (f == NULL) {
    perror("ps");
    return -1;
  }
  char header[512];
  (void)fgets(header, sizeof(header), f);
  int n = 0;
  while ((size_t)n < cap &&
         fscanf(f, "%u %u %15s %15s %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %31s %127s %63s "
                   "%159[^\n]\n",
                &rows[n].pid, &rows[n].ppid, rows[n].state, rows[n].wait, &rows[n].vsz_pages, &rows[n].rss_pages,
                &rows[n].minflt, &rows[n].majflt, &rows[n].cpu_ticks, &rows[n].age_ticks, &rows[n].budget_remaining,
                &rows[n].budget_max, &rows[n].unsupported_syscalls, &rows[n].last_unsupported_syscall,
                &rows[n].unsupported_ioctls, &rows[n].last_unsupported_ioctl, rows[n].name, rows[n].exec_path,
                rows[n].cwd, rows[n].cmdline) == 20) {
    ++n;
  }
  fclose(f);
  return n;
}

static void print_default(const struct proc_row *rows, int n) {
  puts("  PID TTY           TIME CMD");
  for (int i = 0; i < n; ++i) {
    char time[24];
    fmt_time(rows[i].cpu_ticks, time, sizeof(time));
    printf("%5u tty        %8s %s\n", rows[i].pid, time, rows[i].cmdline);
  }
}

static void print_aux(const struct proc_row *rows, int n) {
  unsigned long long total_kib = 0;
  (void)read_mem_total(&total_kib);
  puts("USER               PID  %CPU %MEM      VSZ    RSS   TT  STAT STARTED      TIME COMMAND");
  for (int i = 0; i < n; ++i) {
    unsigned long long rss_kib = rows[i].rss_pages * 4ull;
    unsigned long long vsz_kib = rows[i].vsz_pages * 4ull;
    unsigned long long cpu_tenths = rows[i].age_ticks == 0 ? 0 : (rows[i].cpu_ticks * 1000ull) / rows[i].age_ticks;
    unsigned long long mem_tenths = total_kib == 0 ? 0 : (rss_kib * 1000ull) / total_kib;
    char time[24];
    char cpu[8];
    char mem_pct[8];
    char stat[4] = {state_letter(rows[i].state), '\0', '\0', '\0'};
    if (state_letter(rows[i].state) == 'S') { stat[1] = 's'; }
    fmt_time(rows[i].cpu_ticks, time, sizeof(time));
    fmt_percent(cpu_tenths, rows[i].cpu_ticks != 0, cpu, sizeof(cpu));
    fmt_percent(mem_tenths, rss_kib != 0, mem_pct, sizeof(mem_pct));
    printf("%-16s %5u %5s %5s %8llu %6llu %-3s %-4s %-8s %8s %s\n", "root", rows[i].pid, cpu, mem_pct, vsz_kib, rss_kib,
           "tty", stat, "boot", time, rows[i].cmdline);
  }
}

int main(int argc, char **argv) {
  bool aux = false;
  if (argc > 1) {
    if (argc == 2 && streq(argv[1], "aux")) {
      aux = true;
    } else {
      return usage("ps", "[aux]");
    }
  }

  struct proc_row rows[64];
  int n = load_rows(rows, sizeof(rows) / sizeof(rows[0]));
  if (n < 0) { return EXIT_FAILURE; }

  if (aux) {
    print_aux(rows, n);
  } else {
    print_default(rows, n);
  }
  return EXIT_SUCCESS;
}
