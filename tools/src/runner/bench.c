#include "run_modes.h"
#include "runner.h"
#include <stdio.h>
#include <string.h>

struct bench_command {
  const char *label;
  const char *command;
};

static const struct bench_command bench_commands[] = {
  {"ant startup", "time ant -e 'console.log(1)' > /dev/null\n"},
  {"ls /bin", "time ls /bin > /dev/null\n"},
  {"du -s /", "time du -s / > /dev/null\n"},
  {"find /etc", "time find /etc > /dev/null\n"},
  {"ls -l usr/local", "time ls -l /usr/local/bin > /dev/null\n"},
  {"dynamic opens", "time curl -V > /dev/null\n"},
  {"curl http", "time curl -s http://10.0.2.2:8080/ > /dev/null\n"},
  {"curl https", "time curl -sS https://example.com/ > /dev/null\n"},
  {"nano --version", "time nano --version > /dev/null\n"},
  {"fs stats", "cat /proc/fsstats\n"},
};

void bench_prompt(struct bench_run *state, int fd) {
  if (state->waiting) {
    double elapsed = now_seconds() - state->started;
    printf("[bench] %-18s %.3fs\n", bench_commands[state->done].label, elapsed);
    fflush(stdout);
    ++state->done;
    state->waiting = false;
  }
  if (state->done < sizeof(bench_commands) / sizeof(bench_commands[0])) {
    printf("[bench] running %-18s\n", bench_commands[state->done].label);
    fflush(stdout);
    state->started = now_seconds();
    write_serial_input(fd, bench_commands[state->done].command, strlen(bench_commands[state->done].command));
    state->waiting = true;
  } else if (!state->shutdown_sent) {
    write_serial_input(fd, "sudo shutdown\n", strlen("sudo shutdown\n"));
    state->shutdown_sent = true;
  }
}
