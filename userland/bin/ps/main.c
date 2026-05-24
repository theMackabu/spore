#include <spore.h>
#include <stdio.h>
#include <stdlib.h>

struct proc_row {
  unsigned pid;
  unsigned ppid;
  char state[16];
  char wait[16];
  unsigned long long rss_pages;
  unsigned long long cpu_ticks;
  unsigned long long age_ticks;
  unsigned long long budget_remaining;
  unsigned long long budget_max;
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

int main(void) {
  FILE *f = fopen("/proc/procinfo", "r");
  if (f == NULL) {
    perror("ps");
    return EXIT_FAILURE;
  }
  char header[160];
  (void)fgets(header, sizeof(header), f);
  puts("PID  PPID  S  WAIT    RSS(K)  CPU  TIME  CWD      CMD");
  struct proc_row p;
  while (fscanf(f, "%u %u %15s %15s %llu %llu %llu %llu %llu %31s %127s %63s %159[^\n]\n", &p.pid, &p.ppid, p.state,
                p.wait, &p.rss_pages, &p.cpu_ticks, &p.age_ticks, &p.budget_remaining, &p.budget_max, p.name,
                p.exec_path, p.cwd, p.cmdline) == 13) {
    printf("%3u  %4u  %c  %-6s  %6llu  %3llu  %4llu  %-7s  %s\n", p.pid, p.ppid, state_letter(p.state), p.wait,
           p.rss_pages * 4, p.cpu_ticks, p.age_ticks, p.cwd, p.cmdline);
  }
  fclose(f);
  return EXIT_SUCCESS;
}
