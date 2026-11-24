#pragma once

#include <stdint.h>

int64_t sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags);
int64_t sys_uname(uint64_t buf, const char *machine);
int64_t sys_sethostname(uint64_t name_addr, uint64_t len);
int64_t sys_sysinfo(uint64_t info_addr);
