#pragma once

constexpr unsigned INIT_MAX_ARGS = 128;
constexpr unsigned INIT_MAX_ENV = 128;

struct init_program {
  const char *path, *cwd;
  const char *argv[INIT_MAX_ARGS], *env[INIT_MAX_ENV];
  unsigned argc, envc;
};
