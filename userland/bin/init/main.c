#include "mycelium.h"

struct unit units[UNIT_CAP];
int control_fd = -1;
bool shutting_down;
const char *start_stack[UNIT_CAP];
int start_depth;
bool defer_console_start = true;

void print_motd(void) {
  int fd = open("/etc/motd", O_RDONLY);
  if (fd < 0) { return; }
  char buf[128];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) { break; }
    (void)write(STDOUT_FILENO, buf, (size_t)n);
  }
  close(fd);
}

static void manager_maintenance(void) {
  reap_children();
  check_timers();
  check_watchdogs();
}

static void add_control_fd(struct pollfd *fds, nfds_t *nfds) {
  if (control_fd < 0) { return; }
  fds[*nfds] = (struct pollfd){.fd = control_fd, .events = POLLIN};
  ++*nfds;
}

static void add_journal_fds(struct pollfd *fds, struct unit **journal_units, nfds_t *nfds) {
  for (size_t i = 0; i < UNIT_CAP && *nfds < UNIT_CAP + 1; ++i) {
    if (!units[i].used || units[i].journal_fd < 0) { continue; }
    journal_units[*nfds] = &units[i];
    fds[*nfds] = (struct pollfd){.fd = units[i].journal_fd, .events = POLLIN | POLLHUP};
    ++*nfds;
  }
}

static nfds_t build_pollset(struct pollfd *fds, struct unit **journal_units) {
  nfds_t nfds = 0;
  add_control_fd(fds, &nfds);
  add_journal_fds(fds, journal_units, &nfds);
  return nfds;
}

static bool control_event_ready(const struct pollfd *fds) {
  return control_fd >= 0 && (fds[0].revents & POLLIN) != 0;
}

static nfds_t first_journal_event_index(void) {
  return control_fd >= 0 ? 1 : 0;
}

static void dispatch_journal_events(struct pollfd *fds, struct unit **journal_units, nfds_t start, nfds_t nfds) {
  for (nfds_t i = start; i < nfds; ++i) {
    if ((fds[i].revents & (POLLIN | POLLHUP)) == 0) { continue; }
    read_journal_fd(journal_units[i]);
  }
}

static void dispatch_poll_events(struct pollfd *fds, struct unit **journal_units, nfds_t nfds) {
  if (control_event_ready(fds)) { handle_control_client(); }
  dispatch_journal_events(fds, journal_units, first_journal_event_index(), nfds);
}

static void supervisor_poll_once(void) {
  struct pollfd fds[UNIT_CAP + 1];
  struct unit *journal_units[UNIT_CAP + 1] = {0};
  nfds_t nfds = build_pollset(fds, journal_units);
  if (poll(fds, nfds, next_poll_timeout_ms()) <= 0) { return; }
  dispatch_poll_events(fds, journal_units, nfds);
}

int main(void) {
  load_environment_file("/etc/environment");
  ensure_dir("/run/mycelium");
  ensure_dir("/var/log/mycelium");
  ensure_dir("/var/lib/mycelium");
  load_units();
  setup_control_socket();

  char err[160] = {0};
  (void)start_unit_name("multi-user.target", err, sizeof(err));
  puts("spore: mycelium starting, reached multi-user.target");
  print_motd();
  fflush(stdout);
  defer_console_start = false;
  (void)start_unit_name("console.service", err, sizeof(err));

  for (;;) {
    manager_maintenance();
    supervisor_poll_once();
  }
}
