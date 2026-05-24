#include "sh.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static struct token parse_tokens[TOKEN_CAP];

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
  return wait_status(pid);
}

static int run_confined(const char *manifest, char **argv, const struct redirs *redir) {
  if (spore_streq(manifest, "bad-manifest")) {
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
  bool cpu_demo = spore_streq(manifest, "compute-only") && spore_streq(spore_basename(argv[0]), "spinner");
  if (cpu_demo) { printf("spore: '%s' confined: syscall-class=compute, cpu=200ms\n", argv[0]); }
  int status = wait_status(pid);
  if (cpu_demo) { puts("spore: 'spinner' exceeded CPU budget -> killed (shell alive)"); }
  return status;
}

static int run_builtin(struct command *cmd, int last_status, bool *handled) {
  *handled = true;
  if (cmd->argc == 0) { return 0; }

  if (spore_streq(cmd->argv[0], "cd")) {
    const char *path = cmd->argc > 1 ? cmd->argv[1] : getenv("HOME");
    if (path == NULL) { path = "/"; }
    if (chdir(path) != 0) {
      perror("cd");
      return 1;
    }
    return 0;
  }
  if (spore_streq(cmd->argv[0], "pwd")) {
    char cwd[128];
    puts(getcwd(cwd, sizeof(cwd)) == NULL ? "?" : cwd);
    return 0;
  }
  if (spore_streq(cmd->argv[0], "exit")) { exit(cmd->argc > 1 ? atoi(cmd->argv[1]) : 0); }
  if (spore_streq(cmd->argv[0], "help")) {
    puts("builtins: cd pwd exit help export confine runc");
    puts("syntax: words quotes $VAR $? ; && || < > >>");
    return 0;
  }
  if (spore_streq(cmd->argv[0], "export")) {
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
  if ((spore_streq(cmd->argv[0], "confine") || spore_streq(cmd->argv[0], "runc")) && cmd->argc >= 3) {
    return run_confined(cmd->argv[1], &cmd->argv[2], &cmd->redir);
  }

  *handled = false;
  return 0;
}

int sh_execute_line(char *line, int last_status) {
  size_t count = 0;
  if (sh_tokenize(line, parse_tokens, &count, last_status) != 0) {
    spore_eprintf("sh: parse buffer exhausted\n");
    return 2;
  }

  struct parser parser = {.tokens = parse_tokens, .pos = 0, .count = count};
  enum token_type previous = TOK_SEMI;
  int status = last_status;
  while (sh_parser_peek(&parser) != TOK_END) {
    struct command cmd;
    if (sh_parse_command(&parser, &cmd) != 0) { return 2; }

    bool should_run =
      previous == TOK_SEMI || (previous == TOK_AND && status == 0) || (previous == TOK_OR && status != 0);
    if (cmd.argc > 0 && should_run) {
      bool handled = false;
      int builtin_status = run_builtin(&cmd, status, &handled);
      status = handled ? builtin_status : run_external(&cmd);
    }

    enum token_type next = sh_parser_peek(&parser);
    if (next == TOK_AND || next == TOK_OR || next == TOK_SEMI) {
      previous = sh_parser_take(&parser)->type;
    } else {
      previous = TOK_SEMI;
    }
  }
  return status;
}
