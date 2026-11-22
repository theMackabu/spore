#pragma once

#include <stdbool.h>
#include <stddef.h>

enum scripted_input {
  SCRIPT_NONE,
  SCRIPT_EDIT_MOTD,
  SCRIPT_EDIT_PASTE,
  SCRIPT_PICO_SIMPLE,
  SCRIPT_PICO_PASTE,
  SCRIPT_NANO_PASTE,
  SCRIPT_ANT_FIRST_INTERRUPT,
  SCRIPT_ANT_SECOND_INTERRUPT,
  SCRIPT_ANT_EXIT,
};

struct shell_check {
  size_t sent;
  enum scripted_input input;
  bool nudge_pending;
  double last_output;
};

bool shell_check_output(struct shell_check *state, int fd, char *buf, size_t *len);
void shell_check_idle(struct shell_check *state, int fd, char *buf, size_t *len);

struct bench_run {
  size_t done;
  bool waiting;
  bool shutdown_sent;
  double started;
};

void bench_prompt(struct bench_run *state, int fd);

struct rng_run {
  int stage;
  char sample[256];
};

void rng_prompt(struct rng_run *state, int fd, const char *buf);

void print_guest_checks(const char *buf);
bool answer_guest_stdin(int fd, const char *output);
