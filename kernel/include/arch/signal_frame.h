#pragma once

#include <stdbool.h>
#include <stdint.h>

struct siginfo_fields64;
struct signal_action;
struct thread;
struct trap_frame;

bool arch_push_signal_frame(struct thread *thread, const struct signal_action *action, const struct siginfo_fields64 *info);
int cell_rt_sigreturn(struct trap_frame *frame);
void cell_dump_current_fault(const struct trap_frame *frame, uint64_t far);
