#include "proc/thread.h"
#include "proc/memory.h"
#include "abi/freebsd/dispatch.h"
#include "lib/mem.h"
#include "net/unix.h"
#include "proc/accounting.h"
#include "proc/fd.h"
#include "proc/domain.h"

enum { WAIT_WORDS = (MAX_THREADS + 63) / 64 };
static_assert(WAIT_WORDS <= 64);

static struct wait_set {
  uint64_t words[WAIT_WORDS];
  uint64_t nonempty;
} waiters[WAIT_REASON_COUNT];

void cell_mark_blocked(struct thread *thread) {
  size_t slot = thread - cell_thread_slot(0);
  struct wait_set *set = &waiters[thread->wait.reason];
  set->words[slot / 64] |= UINT64_C(1) << (slot % 64);
  set->nonempty |= UINT64_C(1) << (slot / 64);
  thread->state = THREAD_BLOCKED;
}

static void remove_waiter(struct thread *thread) {
  if (thread->wait.reason == WAIT_NONE) { return; }
  size_t slot = thread - cell_thread_slot(0);
  struct wait_set *set = &waiters[thread->wait.reason];
  size_t word = slot / 64;
  set->words[word] &= ~(UINT64_C(1) << (slot % 64));
  if (!set->words[word]) { set->nonempty &= ~(UINT64_C(1) << word); }
}

struct thread *cell_next_waiter(enum wait_reason reason, size_t *cursor) {
  const struct wait_set *set = &waiters[reason];
  size_t word = *cursor / 64;
  if (!set->nonempty || word >= WAIT_WORDS) { return NULL; }
  uint64_t bits = set->words[word] & (UINT64_MAX << (*cursor % 64));
  if (!bits) {
    uint64_t later = set->nonempty & ((UINT64_MAX - 1) << word);
    if (!later) { return NULL; }
    word = (size_t)__builtin_ctzll(later);
    bits = set->words[word];
  }
  size_t slot = 64 * word + (size_t)__builtin_ctzll(bits);
  *cursor = slot + 1;
  return cell_thread_slot(slot);
}

void cell_wait_reset(void) {
  kmemset(waiters, 0, sizeof(waiters));
}

void cell_clear_wait(struct thread *thread) {
  remove_waiter(thread);
  if (thread->wait.reason == WAIT_FIFO_OPEN) { cell_cancel_fd_reservation(thread->domain, thread->wait.target); }
  if (thread->wait.restore_signal_mask) { thread->signal_mask = thread->wait.saved_signal_mask; }
  struct open_file *file = thread->wait.held_file;
  if (thread->wait.reason == WAIT_EVENTFD) { --file->eventfd_waiters; }
  if (thread->wait.reason == WAIT_SOCKET) { --file->socket_waiters; }
  struct unix_rights *rights = thread->wait.reason == WAIT_PIPE && thread->wait.socket_msg ? thread->wait.unix_rights : NULL;
  thread->wait = (struct thread_wait){.target = -1};
  unix_release_rights(rights);
  if (file != NULL) { cell_release_open_file(file); }
}

void cell_resume_wait(struct thread *thread) {
  cell_clear_wait(thread);
  thread->state = THREAD_RUNNABLE;
}

void cell_finish_wait(struct thread *thread, int64_t result) {
  if (cell_domain_as(thread->domain)->freebsd_abi) { freebsd_result(&thread->tf, result); }
  else { trap_set_result(&thread->tf, (uint64_t)result); }
  cell_resume_wait(thread);
}

int cell_block_current(struct trap_frame *frame, struct thread_wait wait) {
  struct thread *thread = cell_current_thread_internal();
  if (wait.reason == WAIT_SOCKET || wait.reason == WAIT_PIPE) {
    wait.held_file = cell_file_at(thread->domain, wait.target);
    cell_retain_open_file(wait.held_file);
    if (wait.reason == WAIT_SOCKET) { ++wait.held_file->socket_waiters; }
  }
  thread->wait = wait;
  thread->running_cpu = -1;
  cell_mark_blocked(thread);
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_block_current_on_pipe(int fd, uint64_t buf, uint64_t len, bool write, struct trap_frame *frame) {
  return cell_block_current(frame, (struct thread_wait){.reason = WAIT_PIPE, .target = fd, .buf = buf, .len = len,
                                                        .write = write});
}

int cell_block_current_on_sleep(uint64_t deadline_tick, struct trap_frame *frame) {
  return cell_block_current(frame, (struct thread_wait){.reason = WAIT_SLEEP, .target = -1,
                                                        .has_deadline = true, .deadline_tick = deadline_tick});
}

int cell_block_current_on_socket_accept(int fd, uint64_t addr, uint64_t addrlen, int flags, struct trap_frame *frame) {
  return cell_block_current(frame, (struct thread_wait){.reason = WAIT_SOCKET, .target = fd, .socket_addr = addr,
                                                        .socket_addrlen = addrlen, .socket_accept_flags = flags});
}

static int block_socket(struct trap_frame *frame, struct thread_wait wait, uint64_t timeout_ticks) {
  wait.reason = WAIT_SOCKET;
  if (timeout_ticks != 0) {
    wait.has_deadline = true;
    wait.deadline_tick = cell_uptime_ticks() + timeout_ticks;
  }
  return cell_block_current(frame, wait);
}

int cell_block_current_on_socket_flags_timeout(int fd, uint64_t buf, uint64_t len, uint64_t addr, uint64_t addrlen,
                                               uint32_t flags, uint64_t timeout_ticks, struct trap_frame *frame) {
  return block_socket(frame, (struct thread_wait){.target = fd, .buf = buf, .len = len, .socket_addr = addr,
                                                  .socket_addrlen = addrlen, .socket_flags = flags}, timeout_ticks);
}

int cell_block_current_on_socket_write_timeout(int fd, uint64_t buf, uint64_t len, uint64_t timeout_ticks,
                                               struct trap_frame *frame) {
  return block_socket(frame, (struct thread_wait){.target = fd, .buf = buf, .len = len, .write = true}, timeout_ticks);
}

int cell_block_current_on_socket_sendmsg_timeout(int fd, uint64_t msg_addr, uint64_t timeout_ticks,
                                                 struct trap_frame *frame) {
  return block_socket(frame, (struct thread_wait){.target = fd, .socket_msg = true, .write = true,
                                                  .socket_msg_addr = msg_addr}, timeout_ticks);
}

int cell_block_current_on_socket_msg_timeout(int fd, uint64_t msg_addr, uint64_t iov, uint64_t iovlen, uint64_t addr,
                                             uint64_t addrlen, uint32_t flags, uint64_t timeout_ticks,
                                             struct trap_frame *frame) {
  return block_socket(frame, (struct thread_wait){.target = fd, .socket_msg = true, .socket_msg_addr = msg_addr,
                                                  .socket_iov = iov, .socket_iovlen = iovlen, .socket_addr = addr,
                                                  .socket_addrlen = addrlen, .socket_flags = flags}, timeout_ticks);
}
