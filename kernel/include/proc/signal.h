#pragma once

#include <stdbool.h>
#include <stdint.h>

struct domain;
struct thread;
struct trap_frame;

struct signal_action {
  uint64_t handler;
  uint64_t flags;
  uint64_t restorer;
  uint64_t mask;
};

int cell_set_parent_death_signal(struct thread *thread, uint64_t signal);
void cell_free_parent_notifications(struct domain *domain);
struct siginfo_fields64 cell_pending_signal_info(struct thread *thread, int signal, bool consume);
void cell_signal_child_exit(struct domain *parent, struct siginfo_fields64 info);
void cell_terminate_current(int signal, struct trap_frame *frame);
void cell_expire_real_timers(uint64_t now);
void cell_signal_domain(struct domain *domain, int signal, struct trap_frame *frame);
bool cell_deliver_signal_to_thread(struct thread *thread, int signal);
bool cell_deliver_signal_to_thread_fault(struct thread *thread, int signal, uint64_t fault_addr, int sig_code);
bool cell_deliver_pending_signals(struct thread *thread);
bool cell_deliver_pending_signals_current(struct trap_frame *frame);
bool cell_signal_current(int signal, struct trap_frame *frame);
bool cell_signal_current_fault(int signal, struct trap_frame *frame, uint64_t fault_addr, int sig_code);
uint64_t cell_sanitize_signal_mask(uint64_t mask);
int cell_kill(int pid, int signal);
int cell_tkill(int tid, int signal);
bool cell_user_signal_supported(int signal);
int cell_tgkill(int pid, int tid, int signal);
