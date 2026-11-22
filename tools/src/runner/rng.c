#include "run_modes.h"
#include "runner.h"
#include <stdio.h>
#include <string.h>

static bool capture_xxd_line(const char *buf, char *out, size_t cap) {
  const char *line = strstr(buf, "00000000:");
  if (line == NULL || cap == 0) { return false; }
  const char *end = strchr(line, '\n');
  size_t len = end == NULL ? strlen(line) : (size_t)(end - line);
  if (len >= cap) { len = cap - 1; }
  memcpy(out, line, len);
  out[len] = '\0';
  return true;
}

void rng_prompt(struct rng_run *state, int fd, const char *buf) {
  if (state->stage == 0) {
    write_serial_input(fd, "xxd -l 32 /dev/urandom\n", strlen("xxd -l 32 /dev/urandom\n"));
    state->stage = 1;
  } else if (state->stage == 1) {
    (void)capture_xxd_line(buf, state->sample, sizeof(state->sample));
    write_serial_input(fd, "sudo shutdown\n", strlen("sudo shutdown\n"));
    state->stage = 2;
  }
}
