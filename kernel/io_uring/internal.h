#pragma once
#include "abi.h"
#include "io_uring/ring.h"
#include "io_uring/syscalls.h"
#include "proc/fd.h"

struct open_file *uring_file(int fd);
bool uring_opcode_supported(unsigned op);
