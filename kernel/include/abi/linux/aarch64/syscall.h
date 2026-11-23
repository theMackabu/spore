#pragma once

#include <stdint.h>

struct trap_frame;
struct user_address_space;

void syscall_set_address_space(struct user_address_space *as);
void syscall_set_boot_time(uint64_t epoch_sec);
void linux_aarch64_syscall(struct trap_frame *frame);
