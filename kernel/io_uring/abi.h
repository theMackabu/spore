#pragma once
#include <stdint.h>

enum {
  SETUP_CQSIZE = 1u << 3,
  SETUP_CLAMP = 1u << 4,
  SETUP_R_DISABLED = 1u << 6,
  SETUP_NO_SQARRAY = 1u << 16,
  SINGLE_MMAP = 1,
  GETEVENTS = 1,
  ENTER_EXT_ARG = 1u << 3,
  SETUP_SQE128 = 1u << 10,
  SETUP_CQE32 = 1u << 11,
  SETUP_SINGLE_ISSUER = 1u << 12,
  FEAT_NODROP = 1u << 1,
  FEAT_RW_CUR_POS = 1u << 3,
  FEAT_EXT_ARG = 1u << 8,
  FEAT_RSRC_TAGS = 1u << 10,
  OFF_CQ_RING = 0x8000000,
  OFF_SQES = 0x10000000,
#define URING_OP(name, number, syscall, scheduling, file_index) OP_##name = number,
#include "operations.def"
#undef URING_OP
  OP_LAST = OP_PIPE,
  REGISTER_PROBE = 8,
  MAX_ENTRIES = 32768,
  EBADF = 9,
  ENOMEM = 12,
  EFAULT = 14,
  EBUSY = 16,
  EINVAL = 22,
};

struct offsets {
  uint32_t head, tail, mask, entries, flags_or_overflow, dropped_or_cqes, array_or_flags, reserved;
  uint64_t addr;
};
struct params {
  uint32_t sq_entries, cq_entries, flags, cpu, idle, features, wq_fd, reserved[3];
  struct offsets sq, cq;
};
struct rings {
  uint32_t sq_head, sq_tail, sq_mask, sq_entries, sq_flags, dropped;
  uint32_t cq_head, cq_tail, cq_mask, cq_entries, overflow, cq_flags;
};
struct sqe {
  uint8_t opcode, flags;
  uint16_t priority;
  int32_t fd;
  uint64_t off, addr;
  uint32_t len, op_flags;
  uint64_t data;
  uint16_t buf_index, personality;
  uint32_t file_index;
  uint64_t addr3, pad;
};
struct cqe {
  uint64_t data;
  int32_t result;
  uint32_t flags;
};
static_assert(sizeof(struct params) == 120 && sizeof(struct sqe) == 64 && sizeof(struct cqe) == 16);
