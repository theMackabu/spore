#include "proc/futex.h"

#include "arch/aarch64/regs.h"
#include "proc/accounting.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/thread.h"

enum {
  EAGAIN = 11,
  EFAULT = 14,
  EINVAL = 22,
  ETIMEDOUT = 110,
  ROBUST_LIST_LIMIT = 16,
  FUTEX_OWNER_DIED = 0x40000000,
};

struct robust_list_head64 {
  uint64_t next;
  int64_t futex_offset;
  uint64_t pending;
};

int cell_futex_wake_domain(struct domain *domain, uint64_t uaddr, uint32_t count, uint32_t bitset) {
  int woke = 0;
  size_t cursor = 0;
  struct thread *thread;
  while ((uint32_t)woke < count && (thread = cell_next_waiter(WAIT_FUTEX, &cursor))) {
    if (thread->domain == domain && thread->wait.futex_addr == uaddr && (thread->wait.futex_bitset & bitset)) {
      cell_finish_wait(thread, 0);
      ++woke;
    }
  }
  return woke;
}

int cell_futex_requeue(uint64_t from, uint64_t to, int32_t wake_count, int32_t move_count) {
  struct domain *domain = cell_current_domain_internal();
  if (!domain || ((from | to) & 3u) || wake_count < 0 || move_count < 0) { return -EINVAL; }
  if (!cell_ensure_user_range(from, sizeof(uint32_t), VMM_ACCESS_READ) ||
      !cell_ensure_user_range(to, sizeof(uint32_t), VMM_ACCESS_READ)) { return -EFAULT; }
  int woke = cell_futex_wake_domain(domain, from, wake_count, UINT32_MAX);
  int moved = 0;
  size_t cursor = 0;
  struct thread *thread;
  while (moved < move_count && (thread = cell_next_waiter(WAIT_FUTEX, &cursor))) {
    if (thread->domain == domain && thread->wait.futex_addr == from) {
      thread->wait.futex_addr = to;
      ++moved;
    }
  }
  return woke + moved;
}

static void robust_wake_entry(struct domain *domain, uint64_t entry, int64_t futex_offset) {
  uint64_t futex_addr = (uint64_t)((int64_t)entry + futex_offset);
  if ((futex_addr & 3u) != 0 || !cell_ensure_user_range(futex_addr, sizeof(uint32_t), VMM_ACCESS_WRITE)) { return; }
  uint32_t word = 0;
  if (!vmm_copy_from_user(cell_domain_as(domain), &word, futex_addr, sizeof(word))) { return; }
  word |= FUTEX_OWNER_DIED;
  (void)vmm_copy_to_user(cell_domain_as(domain), futex_addr, &word, sizeof(word));
  (void)cell_futex_wake_domain(domain, futex_addr, 1, UINT32_MAX);
}

void cell_futex_cleanup_robust_list(struct thread *thread) {
  struct domain *domain = thread->domain;
  if (domain == NULL || thread->robust_list == 0 ||
      !cell_ensure_user_range(thread->robust_list, sizeof(struct robust_list_head64), VMM_ACCESS_READ)) {
    return;
  }
  struct robust_list_head64 head;
  if (!vmm_copy_from_user(cell_domain_as(domain), &head, thread->robust_list, sizeof(head))) { return; }
  if (head.pending != 0) { robust_wake_entry(domain, head.pending, head.futex_offset); }
  uint64_t node = head.next;
  for (size_t i = 0; i < ROBUST_LIST_LIMIT && node != 0 && node != thread->robust_list; ++i) {
    uint64_t next = 0;
    if (!cell_ensure_user_range(node, sizeof(next), VMM_ACCESS_READ) ||
        !vmm_copy_from_user(cell_domain_as(domain), &next, node, sizeof(next))) {
      break;
    }
    robust_wake_entry(domain, node, head.futex_offset);
    node = next;
  }
}

int cell_set_robust_list_current(uint64_t robust_list) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL) { return -EINVAL; }
  thread->robust_list = robust_list;
  return 0;
}

int cell_futex_wait_current(uint64_t uaddr, uint32_t expected, uint32_t bitset, bool has_deadline, uint64_t deadline_tick,
                            struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  struct thread *thread = cell_current_thread_internal();
  if (domain == NULL || thread == NULL || (uaddr & 3u) != 0) { return -EINVAL; }
  if (!cell_ensure_user_range(uaddr, sizeof(uint32_t), VMM_ACCESS_READ)) { return -EFAULT; }
  uint32_t actual = 0;
  if (!vmm_copy_from_user(cell_domain_as(domain), &actual, uaddr, sizeof(actual))) { return -EFAULT; }
  if (actual != expected) { return -EAGAIN; }
  if (has_deadline && deadline_tick <= cell_uptime_ticks()) { return -ETIMEDOUT; }
  return cell_block_current(frame, (struct thread_wait){
                                       .reason = WAIT_FUTEX,
                                       .target = -1,
                                       .has_deadline = has_deadline,
                                       .deadline_tick = deadline_tick,
                                       .futex_addr = uaddr,
                                       .futex_bitset = bitset,
                                     });
}

int cell_futex_wake_current(uint64_t uaddr, uint32_t count, uint32_t bitset) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || (uaddr & 3u) != 0) { return -EINVAL; }
  if (count == 0) { return 0; }
  return cell_futex_wake_domain(domain, uaddr, count, bitset);
}
