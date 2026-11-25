#pragma once

#include <stdbool.h>
#include <stdint.h>

struct thread;
struct user_namespace;
struct net_namespace {
  uint64_t ino;
  unsigned refs;
  bool lo_up, lo_address;
  uint64_t lo_packets, lo_bytes;
  struct user_namespace *owner;
};

static inline bool netns_can_route(const struct net_namespace *ns, uint32_t ip) {
  return !ns || (ns->lo_up && ns->lo_address && (ip & 0xffu) == 127u);
}

constexpr uint64_t CLONE_NEWNET = 0x40000000;
int netns_create(struct thread *creator, struct net_namespace **out);
struct net_namespace *netns_retain(struct net_namespace *ns);
void netns_release(struct net_namespace *ns);
