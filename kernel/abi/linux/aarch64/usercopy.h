#pragma once

#include "mm/vmm.h"

struct user_address_space *syscall_active_as(void);
bool syscall_user_readable(uint64_t buf, uint64_t len);
bool syscall_user_writable(uint64_t buf, uint64_t len);
int64_t syscall_read_user_string(uint64_t user, char *dst, size_t limit);
bool syscall_copy_string_from_user(uint64_t user, char *dst, size_t cap);
