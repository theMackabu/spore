#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;
struct thread;
struct id_map { uint32_t outer, inner; bool mapped; };
struct user_namespace {
  unsigned refs, depth;
  struct user_namespace *parent;
  uint64_t ino;
  bool groups_denied;
  struct id_map uid, gid;
};
constexpr uint64_t CLONE_NEWUSER = 0x10000000;
constexpr uint32_t USER_OVERFLOW_ID = 65534;

bool userns_init(size_t domains);
struct user_namespace *userns_retain(struct user_namespace *ns);
bool userns_contains(const struct user_namespace *ancestor, const struct user_namespace *child);
void userns_release(struct user_namespace *ns);
int cell_unshare_namespaces(struct thread *thread, uint64_t flags);
uint32_t userns_visible(const struct user_namespace *ns, bool group, uint32_t outer);
bool userns_translate(const struct user_namespace *ns, bool group, uint32_t inner, uint32_t *outer);
int userns_write_map(struct domain *actor, struct user_namespace *target, bool group, bool setgroups,
                     const char *text);
