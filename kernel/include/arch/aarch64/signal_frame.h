#pragma once

#include <stdbool.h>
#include <stdint.h>

struct signal_action;
struct thread;
struct trap_frame;

bool aarch64_push_signal_frame(struct thread *thread, const struct signal_action *action, int signal,
                               uint64_t fault_addr, int sig_code);
int cell_rt_sigreturn(struct trap_frame *frame);
void cell_dump_current_fault(const struct trap_frame *frame, uint64_t far);
