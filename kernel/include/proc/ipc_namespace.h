#pragma once

#include <stdint.h>

struct thread;
struct user_namespace;
struct ipc_namespace {
  uint64_t ino;
  unsigned refs;
  struct user_namespace *owner;
};

constexpr uint64_t CLONE_NEWIPC = 0x08000000;
int ipcns_create(struct thread *creator, struct ipc_namespace **out);
struct ipc_namespace *ipcns_retain(struct ipc_namespace *ns);
void ipcns_release(struct ipc_namespace *ns);
