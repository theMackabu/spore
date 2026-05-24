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

static long load_procs(struct proc_row *infos, size_t cap) {
  FILE *f = fopen("/proc/procinfo", "r");
  if (f == NULL) { return -1; }
  char header[160];
  (void)fgets(header, sizeof(header), f);
  long n = 0;
  while ((size_t)n < cap &&
         fscanf(f, "%u %u %15s %15s %llu %llu %llu %llu %llu %31s %127s %63s %159[^\n]\n", &infos[n].pid,
                &infos[n].ppid, infos[n].state, infos[n].wait, &infos[n].rss_pages, &infos[n].cpu_ticks,
                &infos[n].age_ticks, &infos[n].budget_remaining, &infos[n].budget_max, infos[n].name,
                infos[n].exec_path, infos[n].cwd, infos[n].cmdline) == 13) {
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

  unsigned rows = screen_rows();
  printf("\033[H\033[2J");
  printf("\033[7m Spore top - %ld process%s  (q to quit) \033[m\r\n", n, n == 1 ? "" : "es");
  printf("PID  PPID  STATE    RSS(K)  CPU  AGE  BUDGET       CMD\r\n");
  unsigned used_rows = 2;
  for (long i = 0; i < n && used_rows + 1 < rows; ++i, ++used_rows) {
    char budget[32];
    if (infos[i].budget_max == 0) {
      snprintf(budget, sizeof(budget), "unlimited");
    } else {
      snprintf(budget, sizeof(budget), "%llu/%llu", infos[i].budget_remaining, infos[i].budget_max);
    }
    printf("%3u  %4u  %-7s  %6llu  %3llu  %3llu  %-11s  %s\r\n", infos[i].pid, infos[i].ppid, infos[i].state,
           infos[i].rss_pages * 4, infos[i].cpu_ticks, infos[i].age_ticks, budget, infos[i].cmdline);
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
