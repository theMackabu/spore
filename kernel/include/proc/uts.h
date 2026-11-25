#pragma once

#include <stdbool.h>
#include <stddef.h>

struct domain;
struct thread;
struct uts_namespace;
enum { UTS_NAME_SIZE = 65, CLONE_NEWUTS = 0x04000000 };

bool uts_init(size_t domains);
struct uts_namespace *uts_retain(struct uts_namespace *ns);
void uts_release(struct uts_namespace *ns);
bool uts_may_set(const struct domain *domain);
int uts_unshare(struct thread *leader);
char *uts_name(struct domain *domain, bool hostname);
