#pragma once

#include <stdint.h>

long integration_raw_syscall3(long nr, long a0, long a1, long a2);
long integration_raw_syscall4(long nr, long a0, long a1, long a2, long a3);
long integration_raw_syscall5(long nr, long a0, long a1, long a2, long a3, long a4);
long integration_raw_syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
void integration_exit_group(int code) __attribute__((noreturn));
uint64_t integration_usec_now(void);
void integration_sleep_ms(long ms);

int integration_snapshot_regression(void);
int integration_timer_budget_demo(void);
int integration_exec_demo(void);
int integration_exec_private_data(void);
int integration_proc_self_links(void);
int integration_stdin_demo(void);
int integration_thread_demo(void);
int integration_futex_demo(void);
int integration_pty_demo(void);
int integration_pthread_demo(void);
int integration_udp_demo(void);
int integration_socket_suite(void);
int integration_egress_demo(void);
int integration_seccomp(void);
int integration_uts_namespace(void);
int integration_mount_namespace(void);
int integration_user_namespace(void);
int integration_capabilities(void);
int integration_signal_suite(void);
int integration_memory_suite(void);
int integration_fork_suite(void);
int integration_fs_demo(void);
int integration_notify_suite(void);
int integration_fifo_suite(void);
int integration_open_suite(void);
int integration_tmpfile(const char *directory);
