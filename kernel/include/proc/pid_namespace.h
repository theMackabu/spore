#pragma once
#include <stdbool.h>
#include <stdint.h>
struct thread;
struct domain;
struct pid_entry;
struct pid_namespace;

constexpr uint64_t CLONE_NEWPID = 0x20000000;
int pidns_create(struct thread *thread, struct pid_namespace **out);
struct pid_namespace *pidns_retain(struct pid_namespace *ns);
void pidns_release(struct pid_namespace *ns);
uint64_t pidns_ino(const struct pid_namespace *ns);
struct pid_namespace *pidns_active(const struct domain *domain);
struct pid_namespace *pidns_current(void);
int pidns_allocate(struct pid_namespace *ns, int global, struct pid_entry **out);
struct pid_entry *pidns_hold_id(struct pid_namespace *ns, int global);
struct pid_entry *pidns_retain_id(struct pid_entry *id);
bool pidns_init_process(const struct domain *d);
int pidns_last(const struct pid_namespace *ns);
void pidns_drop_id(struct pid_entry *id);
int pidns_local(const struct pid_namespace *ns, int global);
int pidns_global(const struct pid_namespace *ns, int local);
int pidns_self_view(int global);
int pidns_reaper(const struct domain *domain);
bool pidns_mark_dead(struct domain *domain);
bool pidns_has_dead_parent(const struct domain *domain);
