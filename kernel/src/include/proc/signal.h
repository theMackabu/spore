#pragma once

#include "cell.h"

void cell_expire_real_timers(uint64_t now);
void cell_signal_domain(struct domain *domain, int signal, struct trap_frame *frame);
bool cell_deliver_signal_to_thread(struct thread *thread, int signal);
bool cell_deliver_signal_to_thread_fault(struct thread *thread, int signal, uint64_t fault_addr, int sig_code);
bool cell_deliver_pending_signals(struct thread *thread);
bool cell_deliver_pending_signals_current(struct trap_frame *frame);
