#pragma once

#include <stdbool.h>
#include <stdint.h>

enum linux_capability {
  CAP_CHOWN = 0, CAP_DAC_OVERRIDE = 1, CAP_DAC_READ_SEARCH = 2, CAP_FOWNER = 3, CAP_FSETID = 4,
  CAP_KILL = 5, CAP_SETGID = 6, CAP_SETUID = 7, CAP_SETPCAP = 8, CAP_NET_ADMIN = 12,
  CAP_SYS_PTRACE = 19, CAP_SYS_CHROOT = 18, CAP_SYS_ADMIN = 21, CAP_SYS_BOOT = 22, CAP_SYS_RESOURCE = 24,
};

struct linux_caps {
  uint32_t effective, permitted, inheritable, bounding;
};

struct domain;
struct user_namespace;
bool cell_ns_capable(const struct user_namespace *owner, enum linux_capability cap);
struct linux_caps cell_initial_caps(void);
bool cell_capable(enum linux_capability cap);
int cell_cap_set(uint32_t effective, uint32_t permitted, uint32_t inheritable);
int cell_cap_bound(bool drop, unsigned cap);
void cell_caps_after_setuid(struct domain *d, uint32_t old_euid, bool had_root);
void cell_caps_after_exec(const struct domain *d);
