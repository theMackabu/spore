#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum { PATH_CAP = 1024 };

void write_serial_input(int fd, const char *buf, size_t len);
double now_seconds(void);
ssize_t find_shell_prompt(const char *data, size_t len);
int run_harness(char **qemu_argv, const char *mode, bool timings);
void restore_host_terminal_mouse(void);
pid_t start_udp_echo_server(void);
pid_t start_http_server(void);
