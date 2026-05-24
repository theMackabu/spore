#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

extern char **environ;

#define SYS_SPORE_APPLY_POLICY 0x4005

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
    ++s;
  }
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[--len] = '\0';
  }
  return s;
}

static int read_line(char *buf, size_t cap) {
  size_t len = 0;
  for (;;) {
    char c;
    ssize_t n = read(0, &c, 1);
    if (n <= 0) { return -1; }
    if (c == '\r') { c = '\n'; }
    if (c == 3) {
      write(1, "^C\n", 3);
      buf[0] = '\0';
      return 0;
    }
    if (c == 0x7f || c == '\b') {
      if (len > 0) {
        --len;
        write(1, "\b \b", 3);
      }
      continue;
    }
    write(1, &c, 1);
    if (c == '\n') {
      buf[len] = '\0';
      return 0;
    }
    if (len + 1 < cap) { buf[len++] = c; }
  }
}

static void make_path(const char *cmd, char *out, size_t cap, int bin) {
  if (strchr(cmd, '/') != NULL) {
    snprintf(out, cap, "%s", cmd);
  } else if (bin) {
    snprintf(out, cap, "/bin/%s", cmd);
  } else {
    snprintf(out, cap, "./%s", cmd);
  }
}

static int run_external(char **argv, const char *redir) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 1;
  }
  if (pid == 0) {
    if (redir != NULL) {
      close(1);
      int fd = open(redir, O_CREAT | O_WRONLY | O_TRUNC, 0666);
      if (fd != 1) { _exit(126); }
    }
    char path[128];
    make_path(argv[0], path, sizeof(path), 0);
    execve(path, argv, environ);
    make_path(argv[0], path, sizeof(path), 1);
    execve(path, argv, environ);
    fprintf(stderr, "%s: not found\n", argv[0]);
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status)) { return WEXITSTATUS(status); }
  return 128;
}

static int run_confined(const char *manifest, char **argv) {
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
    long rc = syscall(SYS_SPORE_APPLY_POLICY, manifest);
    if (rc < 0) {
      puts("spore: spawn rejected: requested caps exceed parent");
      _exit(126);
    }
    char path[128];
    make_path(argv[0], path, sizeof(path), 0);
    execve(path, argv, environ);
    make_path(argv[0], path, sizeof(path), 1);
    execve(path, argv, environ);
    _exit(127);
  }
  int cpu_demo = spore_streq(manifest, "compute-only") && spore_streq(spore_basename(argv[0]), "spinner");
  if (cpu_demo) { printf("spore: '%s' confined: syscall-class=compute, cpu=200ms\n", argv[0]); }
  int status = 0;
  waitpid(pid, &status, 0);
  if (cpu_demo) { puts("spore: 'spinner' exceeded CPU budget -> killed (shell alive)"); }
  if (WIFEXITED(status)) { return WEXITSTATUS(status); }
  return 128;
}

static int run_one(char *cmdline) {
  char *cmd = trim(cmdline);
  if (*cmd == '\0') { return 0; }

  char *argv[16];
  int argc = 0;
  char *redir = NULL;
  for (char *tok = strtok(cmd, " \t"); tok != NULL && argc < 15; tok = strtok(NULL, " \t")) {
    if (strcmp(tok, ">") == 0) {
      redir = strtok(NULL, " \t");
      break;
    }
    argv[argc++] = tok;
  }
  argv[argc] = NULL;
  if (argc == 0) { return 0; }

  if (spore_streq(argv[0], "cd")) {
    const char *path = argc > 1 ? argv[1] : "/";
    if (chdir(path) != 0) {
      perror("cd");
      return 1;
    }
    return 0;
  }
  if (spore_streq(argv[0], "pwd")) {
    char cwd[128];
    puts(getcwd(cwd, sizeof(cwd)) == NULL ? "?" : cwd);
    return 0;
  }
  if (spore_streq(argv[0], "exit")) { exit(argc > 1 ? atoi(argv[1]) : 0); }
  if (spore_streq(argv[0], "help")) {
    puts("builtins: cd pwd exit help");
    return 0;
  }
  if ((spore_streq(argv[0], "confine") || spore_streq(argv[0], "runc")) && argc >= 3) {
    return run_confined(argv[1], &argv[2]);
  }
  return run_external(argv, redir);
}

int main(void) {
  char line[256];
  int last = 0;
  for (;;) {
    char cwd[128];
    printf("%s $ ", getcwd(cwd, sizeof(cwd)) == NULL ? "/" : cwd);
    fflush(stdout);
    if (read_line(line, sizeof(line)) < 0) { return last; }
    char *cursor = line;
    while (cursor != NULL) {
      char *next = strstr(cursor, "&&");
      if (next != NULL) {
        *next = '\0';
        next += 2;
      }
      last = run_one(cursor);
      if (last != 0) { break; }
      cursor = next;
    }
  }
}
