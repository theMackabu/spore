#pragma once

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spore.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

constexpr int SYS_SPORE_SET_BUDGET = 0x4004;
constexpr int SYS_SPORE_SHUTDOWN = 0x4006;

enum {
  UNIT_CAP = 48,
  DEP_CAP = 12,
  ARG_CAP = 24,
  LOG_LINE = 256,
  LOG_MEM = 2048,
  START_LIMIT_HISTORY = 8,
};

enum unit_type {
  UNIT_SERVICE,
  UNIT_TARGET,
  UNIT_TIMER,
  UNIT_PATH,
  UNIT_SOCKET,
  UNIT_MOUNT,
};

enum service_type {
  SERVICE_SIMPLE,
  SERVICE_FORKING,
  SERVICE_ONESHOT,
  SERVICE_NOTIFY,
  SERVICE_IDLE,
};

enum unit_state {
  STATE_INACTIVE,
  STATE_ACTIVATING,
  STATE_ACTIVE,
  STATE_DEACTIVATING,
  STATE_FAILED,
};

enum restart_policy {
  RESTART_NO,
  RESTART_ON_SUCCESS,
  RESTART_ON_FAILURE,
  RESTART_ON_ABNORMAL,
  RESTART_ON_WATCHDOG,
  RESTART_ALWAYS,
};

struct string_list {
  char items[DEP_CAP][64];
  int count;
};

struct unit_config {
  enum unit_type type;
  enum service_type service_type;
  enum restart_policy restart;
  char description[96];
  char exec_start[192];
  char exec_start_pre[192];
  char exec_start_post[192];
  char exec_stop[192];
  char exec_reload[192];
  char capability[64];
  char user[32];
  char standard_output[96];
  char standard_error[96];
  char wanted_by[64];
  char unit_to_activate[64];
  struct string_list requires_units;
  struct string_list wants;
  struct string_list requisite;
  struct string_list binds_to;
  struct string_list part_of;
  struct string_list conflicts;
  struct string_list after;
  struct string_list before;
  int restart_sec;
  bool restart_immediately;
  int watchdog_sec;
  int timeout_stop_sec;
  int start_limit_interval;
  int start_limit_burst;
  int on_boot_sec;
  int on_unit_active_sec;
  int on_unit_inactive_sec;
  int on_calendar_minute;
  uint64_t cpu_budget_ticks;
  uint64_t memory_pages;
  bool persistent;
};

struct journal_stream {
  int fd;
  size_t used;
  char line[LOG_LINE];
};

struct unit {
  struct unit_config config;
  bool used;
  enum unit_state state;
  char name[64];
  pid_t pid;
  int status;
  struct journal_stream journal;
  time_t active_since;
  time_t inactive_since;
  time_t last_watchdog;
  time_t starts[START_LIMIT_HISTORY];
  int start_count;
  bool boot_reported;
  time_t next_fire;
  char log_mem[LOG_MEM];
  size_t log_len;
  char fail_reason[64];
};

extern struct unit units[UNIT_CAP];
extern int control_fd;
extern bool shutting_down;
extern const char *start_stack[UNIT_CAP];
extern int start_depth;
extern bool defer_console_start;
extern bool boot_status_enabled;

#define MYC_RESET "\033[0m"
#define MYC_GREEN "\033[1;32m"
#define MYC_CYAN "\033[1;36m"
#define MYC_BLUE "\033[1;34m"

const char *state_name(enum unit_state state);
const char *type_name(enum unit_type type);
void copy_text(char *dst, size_t cap, const char *src);
char *trim(char *s);
bool ends_with(const char *s, const char *suffix);
void ensure_dir(const char *path);
void append_response(char *out, size_t cap, const char *fmt, ...);
void append_log_file(const char *path, const char *text);
void system_log_append(const char *text);
void boot_log_append(const char *text);
void sync_kernel_log(bool boot_snapshot);
bool list_has(const struct string_list *list, const char *name);
int split_args(char *cmd, char **argv, int cap);
void load_environment_file(const char *path);
void boot_banner(void);
void boot_statusf(const char *fmt, ...);
void boot_infof(const char *fmt, ...);
void boot_run_login_banner(void);
void announce_unit_started(struct unit *unit);
void append_done_line(char *out, size_t cap, const char *fmt, ...);

struct unit *unit_by_name(const char *name);
struct unit *unit_for_pid(pid_t pid);
struct unit *alloc_unit(const char *name);
void load_units(void);
void journal_append(struct unit *unit, const char *fmt, ...);

int start_unit_name(const char *name, char *err, size_t err_cap);
int run_oneshot_command(struct unit *unit, const char *cmdline);
void stop_unit(struct unit *unit);
void reap_children(void);
void redirect_output(const char *mode, int fd, int fallback_fd);
pid_t fork_unit(struct unit *unit, struct journal_stream *stream);
void journal_read(struct unit *unit, struct journal_stream *stream);
void journal_finish(struct unit *unit, struct journal_stream *stream);
void check_timers(void);
void check_watchdogs(void);
int next_poll_timeout_ms(void);

void handle_control_client(void);
void setup_control_socket(void);
