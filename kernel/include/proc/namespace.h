#pragma once

#include <stdint.h>
struct domain;

enum namespace_kind { NS_MOUNT, NS_USER, NS_UTS, NS_IPC, NS_COUNT };

struct namespace_ref {
  enum namespace_kind kind;
  void *object;
};

uint64_t namespace_new_ino(void);
uint64_t namespace_ino(struct namespace_ref ns);
struct namespace_ref namespace_of(const struct domain *domain, enum namespace_kind kind);
const char *namespace_name(enum namespace_kind kind);
void namespace_retain(struct namespace_ref ns);
void namespace_release(struct namespace_ref ns);
