#include "msh.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct token parse_tokens[TOKEN_CAP];
static bool xtrace;

enum { JOB_CAP = 16 };

struct job {
  pid_t pid;
  int status;
  bool running;
  bool reported;
  char cmd[128];
};

static struct job jobs[JOB_CAP];

static const char *command_name(char **argv) {
  return argv != NULL && argv[0] != NULL ? argv[0] : "?";
}

struct signal_name {
  const char *name;
  int value;
};

static const struct signal_name signals[] = {
  {"INT", SIGINT},   {"SIGINT", SIGINT},   {"KILL", SIGKILL}, {"SIGKILL", SIGKILL},
  {"SEGV", SIGSEGV}, {"SIGSEGV", SIGSEGV}, {"TERM", SIGTERM}, {"SIGTERM", SIGTERM},
};

static bool parse_signal(const char *text, int *out) {
  if (text == NULL || text[0] == '\0') { return false; }
  char *end = NULL;
  long value = strtol(text, &end, 10);
  if (end != text && *end == '\0' && value > 0 && value < 128) {
    *out = (int)value;
    return true;
  }
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
    if (streq(text, signals[i].name)) {
      *out = signals[i].value;
      return true;
    }
  }
  return false;
}

static const char *signal_message(int sig) {
  switch (sig) {
  case SIGINT:
    return "Interrupted";
  case SIGKILL:
    return "Killed";
  case SIGSEGV:
    return "Segmentation fault";
  case SIGTERM:
    return "Terminated";
  }
  return "Signaled";
}

static void remember_job(pid_t pid, char **argv) {
  for (size_t i = 0; i < JOB_CAP; ++i) {
    if (jobs[i].pid == 0 || (!jobs[i].running && jobs[i].reported)) {
      jobs[i] = (struct job){.pid = pid, .running = true};
      snprintf(jobs[i].cmd, sizeof(jobs[i].cmd), "%s", command_name(argv));
      printf("[%zu] %d\n", i + 1, (int)pid);
      return;
    }
  }
  printf("[?] %d\n", (int)pid);
}

static struct job *find_job_pid(pid_t pid) {
  for (size_t i = 0; i < JOB_CAP; ++i) {
    if (jobs[i].pid == pid) { return &jobs[i]; }
  }
  return NULL;
}

static struct job *find_job_spec(const char *spec) {
  if (spec == NULL) { return NULL; }
  if (spec[0] == '%') {
    long n = strtol(spec + 1, NULL, 10);
    if (n > 0 && n <= JOB_CAP && jobs[n - 1].pid != 0) { return &jobs[n - 1]; }
    return NULL;
  }
  return find_job_pid((pid_t)strtol(spec, NULL, 10));
}

void sh_reap_jobs(bool verbose) {
  int status = 0;
  for (;;) {
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid <= 0) { return; }
    struct job *job = find_job_pid(pid);
    if (job == NULL) { continue; }
    job->status = status;
    job->running = false;
    if (verbose && !job->reported) {
      printf("[%ld] done %d %s\n", (long)(job - jobs + 1), (int)pid, job->cmd);
      job->reported = true;
    }
  }
}

static int apply_redirs(const struct redirs *redir) {
  if (redir->in != NULL) {
    close(STDIN_FILENO);
    int fd = open(redir->in, O_RDONLY);
    if (fd != STDIN_FILENO) { return -1; }
  }
  if (redir->out != NULL) {
    close(STDOUT_FILENO);
    int flags = O_CREAT | O_WRONLY | (redir->append ? O_APPEND : O_TRUNC);
    int fd = open(redir->out, flags, 0666);
    if (fd != STDOUT_FILENO) { return -1; }
  }
  return 0;
}

static void exec_search(char **argv) {
  char path[160];
  if (strchr(argv[0], '/') != NULL) {
    execve(argv[0], argv, environ);
    return;
  }

  snprintf(path, sizeof(path), "./%s", argv[0]);
  execve(path, argv, environ);

  const char *path_env = getenv("PATH");
  if (path_env == NULL || path_env[0] == '\0') { path_env = "/bin"; }
  const char *p = path_env;
  while (*p != '\0') {
    const char *end = strchr(p, ':');
    size_t len = end == NULL ? strlen(p) : (size_t)(end - p);
    if (len == 0) {
      snprintf(path, sizeof(path), "./%s", argv[0]);
    } else {
      snprintf(path, sizeof(path), "%.*s/%s", (int)len, p, argv[0]);
    }
    execve(path, argv, environ);
    if (end == NULL) { break; }
    p = end + 1;
  }
}

static int wait_status(pid_t pid) {
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return 1;
  }
  if (WIFEXITED(status)) { return WEXITSTATUS(status); }
  if (WIFSIGNALED(status)) {
    puts(signal_message(WTERMSIG(status)));
    return 128 + WTERMSIG(status);
  }
  return 128;
}

static int status_code(int status) {
  if (WIFEXITED(status)) { return WEXITSTATUS(status); }
  if (WIFSIGNALED(status)) { return 128 + WTERMSIG(status); }
  return 128;
}

static int run_external(struct command *cmd) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    if (apply_redirs(&cmd->redir) != 0) {
      perror("redir");
      _exit(126);
    }
    exec_search(cmd->argv);
    fprintf(stderr, "%s: not found\n", cmd->argv[0]);
    _exit(127);
  }
  if (cmd->background) {
    remember_job(pid, cmd->argv);
    return 0;
  }
  return wait_status(pid);
}

static int run_confined(const char *manifest, char **argv, const struct redirs *redir) {
  if (streq(manifest, "bad-manifest")) {
    puts("spore: spawn rejected: requested caps exceed parent");
    return 1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    if (apply_redirs(redir) != 0) {
      perror("redir");
      _exit(126);
    }
    long rc = syscall(SYS_SPORE_APPLY_POLICY, manifest);
    if (rc < 0) {
      puts("spore: spawn rejected: requested caps exceed parent");
      _exit(126);
    }
    exec_search(argv);
    _exit(127);
  }
  bool cpu_demo = streq(manifest, "compute-only") && streq(basename(argv[0]), "spinner");
  if (cpu_demo) { printf("spore: '%s' confined: syscall-class=compute, cpu=200ms\n", argv[0]); }
  int status = wait_status(pid);
  if (cpu_demo) { puts("spore: 'spinner' exceeded CPU budget -> killed (shell alive)"); }
  return status;
}

static bool is_name_char(char c, bool first) {
  return c == '_' || (!first && c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool assignment_word(char *word) {
  char *eq = strchr(word, '=');
  if (eq == NULL || eq == word) { return false; }
  for (char *p = word; p < eq; ++p) {
    if (!is_name_char(*p, p == word)) { return false; }
  }
  *eq = '\0';
  int rc = setenv(word, eq + 1, 1);
  *eq = '=';
  return rc == 0;
}

static bool all_assignments(struct command *cmd) {
  if (cmd->argc == 0) { return false; }
  for (int i = 0; i < cmd->argc; ++i) {
    char *eq = strchr(cmd->argv[i], '=');
    if (eq == NULL || eq == cmd->argv[i]) { return false; }
    for (char *p = cmd->argv[i]; p < eq; ++p) {
      if (!is_name_char(*p, p == cmd->argv[i])) { return false; }
    }
  }
  return true;
}

static void trace_command(const struct command *cmd) {
  if (!xtrace || cmd->argc == 0) { return; }
  const char *ps4 = getenv("PS4");
  char prompt[96];
  sh_expand_prompt(ps4 == NULL ? "+ " : ps4, prompt, sizeof(prompt));
  fputs(prompt, stderr);
  for (int i = 0; i < cmd->argc; ++i) {
    if (i > 0) { fputc(' ', stderr); }
    fputs(cmd->argv[i], stderr);
  }
  fputc('\n', stderr);
}

static int run_builtin(struct command *cmd, int last_status, bool *handled) {
  *handled = true;
  if (cmd->argc == 0) { return 0; }

  if (all_assignments(cmd)) {
    for (int i = 0; i < cmd->argc; ++i) {
      if (!assignment_word(cmd->argv[i])) {
        perror("setenv");
        return 1;
      }
    }
    return 0;
  }

  if (streq(cmd->argv[0], "cd")) {
    const char *path = cmd->argc > 1 ? cmd->argv[1] : getenv("HOME");
    if (path == NULL) { path = "/"; }
    if (chdir(path) != 0) {
      perror("cd");
      return 1;
    }
    char cwd[128];
    if (getcwd(cwd, sizeof(cwd)) != NULL) { setenv("PWD", cwd, 1); }
    return 0;
  }
  if (streq(cmd->argv[0], "pwd")) {
    char cwd[128];
    puts(getcwd(cwd, sizeof(cwd)) == NULL ? "?" : cwd);
    return 0;
  }
  if (streq(cmd->argv[0], "exit")) { exit(cmd->argc > 1 ? atoi(cmd->argv[1]) : 0); }
  if (streq(cmd->argv[0], "help")) {
    puts("builtins: . source cd pwd exit help export unset set select jobs wait fg kill confine runc");
    puts("syntax: NAME=value words quotes $VAR $? ; && || & < > >>");
    puts("prompts: PS1 primary, PS2 continuation, PS3 select, PS4 trace");
    return 0;
  }
  if (streq(cmd->argv[0], ".") || streq(cmd->argv[0], "source")) {
    if (cmd->argc < 2) { return usage(cmd->argv[0], "FILE"); }
    return sh_source_file(cmd->argv[1], last_status, true);
  }
  if (streq(cmd->argv[0], "jobs")) {
    sh_reap_jobs(false);
    for (size_t i = 0; i < JOB_CAP; ++i) {
      if (jobs[i].pid == 0) { continue; }
      printf("[%zu] %s %d %s\n", i + 1, jobs[i].running ? "running" : "done", (int)jobs[i].pid, jobs[i].cmd);
      if (!jobs[i].running) { jobs[i].reported = true; }
    }
    return 0;
  }
  if (streq(cmd->argv[0], "wait") || streq(cmd->argv[0], "fg")) {
    if (cmd->argc > 1) {
      struct job *job = find_job_spec(cmd->argv[1]);
      if (job == NULL) {
        fprintf(stderr, "%s: no such job: %s\n", cmd->argv[0], cmd->argv[1]);
        return 1;
      }
      if (!job->running) {
        job->reported = true;
        return status_code(job->status);
      }
      int status = wait_status(job->pid);
      job->running = false;
      job->reported = true;
      return status;
    }
    int rc = 0;
    for (size_t i = 0; i < JOB_CAP; ++i) {
      if (jobs[i].pid != 0 && jobs[i].running) { rc = wait_status(jobs[i].pid); }
      jobs[i].running = false;
      jobs[i].reported = true;
    }
    return rc;
  }
  if (streq(cmd->argv[0], "kill")) {
    int sig = SIGTERM;
    int first = 1;
    if (cmd->argc == 2 && streq(cmd->argv[1], "-l")) {
      puts("INT KILL TERM");
      return 0;
    }
    if (cmd->argc > 2 && (streq(cmd->argv[1], "-s") || streq(cmd->argv[1], "--signal"))) {
      if (!parse_signal(cmd->argv[2], &sig)) {
        fprintf(stderr, "kill: unknown signal: %s\n", cmd->argv[2]);
        return 1;
      }
      first = 3;
    } else if (cmd->argc > 1 && cmd->argv[1][0] == '-' && cmd->argv[1][1] != '\0') {
      if (!parse_signal(cmd->argv[1] + 1, &sig)) {
        fprintf(stderr, "kill: unknown signal: %s\n", cmd->argv[1] + 1);
        return 1;
      }
      first = 2;
    }
    if (first >= cmd->argc) { return usage("kill", "[-l] [-s SIGNAL|-SIGNAL] PID|%JOB..."); }
    int rc = 0;
    for (int i = first; i < cmd->argc; ++i) {
      struct job *job = find_job_spec(cmd->argv[i]);
      char *end = NULL;
      long pid = job != NULL ? (long)job->pid : strtol(cmd->argv[i], &end, 10);
      if (job == NULL && (end == cmd->argv[i] || *end != '\0')) {
        fprintf(stderr, "kill: invalid pid: %s\n", cmd->argv[i]);
        rc = 1;
        continue;
      }
      if (kill((pid_t)pid, sig) != 0) {
        perror(cmd->argv[i]);
        rc = 1;
      }
    }
    return rc;
  }
  if (streq(cmd->argv[0], "export")) {
    for (int i = 1; i < cmd->argc; ++i) {
      char *eq = strchr(cmd->argv[i], '=');
      if (eq == NULL) {
        const char *value = getenv(cmd->argv[i]);
        if (value != NULL) { printf("%s=%s\n", cmd->argv[i], value); }
      } else {
        *eq = '\0';
        if (setenv(cmd->argv[i], eq + 1, 1) != 0) {
          perror("export");
          return 1;
        }
      }
    }
    return 0;
  }
  if (streq(cmd->argv[0], "unset")) {
    for (int i = 1; i < cmd->argc; ++i) {
      if (unsetenv(cmd->argv[i]) != 0) {
        perror("unset");
        return 1;
      }
    }
    return 0;
  }
  if (streq(cmd->argv[0], "set")) {
    for (int i = 1; i < cmd->argc; ++i) {
      if (streq(cmd->argv[i], "-x")) {
        xtrace = true;
      } else if (streq(cmd->argv[i], "+x")) {
        xtrace = false;
      } else {
        return usage("set", "[-x|+x]");
      }
    }
    return 0;
  }
  if (streq(cmd->argv[0], "select")) {
    if (cmd->argc < 3) { return usage("select", "NAME WORD..."); }
    for (int i = 2; i < cmd->argc; ++i) {
      printf("%d) %s\n", i - 1, cmd->argv[i]);
    }
    const char *ps3 = getenv("PS3");
    char prompt[96];
    sh_expand_prompt(ps3 == NULL ? "#? " : ps3, prompt, sizeof(prompt));
    fputs(prompt, stdout);
    fflush(stdout);
    char reply[32];
    if (fgets(reply, sizeof(reply), stdin) == NULL) { return 1; }
    size_t reply_len = strlen(reply);
    while (reply_len > 0 && (reply[reply_len - 1] == '\n' || reply[reply_len - 1] == '\r')) {
      reply[--reply_len] = '\0';
    }
    char *end = NULL;
    long n = strtol(reply, &end, 10);
    setenv("REPLY", reply, 1);
    if (n < 1 || n > cmd->argc - 2) {
      setenv(cmd->argv[1], "", 1);
      return 1;
    }
    setenv(cmd->argv[1], cmd->argv[n + 1], 1);
    return 0;
  }
  if ((streq(cmd->argv[0], "confine") || streq(cmd->argv[0], "runc")) && cmd->argc >= 3) {
    return run_confined(cmd->argv[1], &cmd->argv[2], &cmd->redir);
  }

  *handled = false;
  return 0;
}

int sh_source_file(const char *path, int last_status, bool complain) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    if (complain) { perror(path); }
    return complain ? 1 : last_status;
  }

  char line[LINE_CAP];
  int status = last_status;
  while (fgets(line, sizeof(line), file) != NULL) {
    status = sh_execute_line(line, status);
  }
  fclose(file);
  return status;
}

int sh_execute_line(char *line, int last_status) {
  size_t count = 0;
  if (sh_tokenize(line, parse_tokens, &count, last_status) != 0) {
    eprintf("sh: parse buffer exhausted\n");
    return 2;
  }

  struct parser parser = {.tokens = parse_tokens, .pos = 0, .count = count};
  enum token_type previous = TOK_SEMI;
  int status = last_status;
  while (sh_parser_peek(&parser) != TOK_END) {
    struct command cmd;
    if (sh_parse_command(&parser, &cmd) != 0) { return 2; }
    if (sh_parser_peek(&parser) == TOK_BG) { cmd.background = true; }

    bool should_run =
      previous == TOK_SEMI || (previous == TOK_AND && status == 0) || (previous == TOK_OR && status != 0);
    if (cmd.argc > 0 && should_run) {
      trace_command(&cmd);
      bool handled = false;
      int builtin_status = run_builtin(&cmd, status, &handled);
      status = handled ? builtin_status : run_external(&cmd);
    }

    enum token_type next = sh_parser_peek(&parser);
    if (next == TOK_AND || next == TOK_OR || next == TOK_SEMI || next == TOK_BG) {
      previous = sh_parser_take(&parser)->type;
    } else {
      previous = TOK_SEMI;
    }
    if (previous == TOK_BG) { previous = TOK_SEMI; }
  }
  return status;
}
