#include "internal.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pool.h"
#include "proc/epoll.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "proc/thread.h"

enum { MAX_EVENTS = 16384, HEADER = 16, OVERFLOW = 0x4000, NONBLOCK = 04000 };
static_assert(offsetof(struct inotify_event_record, event.name) - offsetof(struct inotify_event_record, event) == HEADER);

static struct object_pool events;
static struct inotify_state *pending;
static bool flushing;

void inotify_enqueue(struct inotify_state *state, int wd, uint32_t mask, uint32_t cookie, const char *name) {
  struct inotify_event_record *tail = state->tail;
  if (tail && tail->event.wd == wd && tail->event.mask == mask && tail->event.cookie == cookie &&
      str_eq(tail->event.name, name ? name : "")) { return; }
  struct inotify_event_record *record = NULL;
  if (state->count < MAX_EVENTS && (events.limit || pool_init(&events, sizeof(*record), SIZE_MAX))) {
    record = pool_alloc(&events);
  }
  if (!record) {
    if (state->overflow_queued) { return; }
    record = &state->overflow;
    state->overflow_queued = true;
    wd = -1;
    mask = OVERFLOW;
    name = NULL;
    cookie = 0;
  }
  record->next = NULL;
  record->event.wd = wd;
  record->event.mask = mask;
  record->event.cookie = cookie;
  if (name) {
    size_t n = kstrlen(name) + 1;
    record->event.len = (uint32_t)((n + HEADER - 1) & ~(size_t)(HEADER - 1));
    kmemcpy(record->event.name, name, n);
  }
  if (tail) { tail->next = record; }
  else { state->head = record; }
  state->tail = record;
  ++state->count;
  state->bytes += HEADER + record->event.len;
  if (!state->pending) {
    state->pending = true;
    state->pending_next = pending;
    pending = state;
    cell_retain_open_file(state->file);
  }
}

static void drop_head(struct inotify_state *state) {
  struct inotify_event_record *record = state->head;
  state->head = record->next;
  if (!state->head) { state->tail = NULL; }
  --state->count;
  state->bytes -= HEADER + record->event.len;
  if (record == &state->overflow) { state->overflow_queued = false; }
  else { pool_free(&events, record); }
}

void inotify_clear_events(struct inotify_state *state) {
  while (state->head) { drop_head(state); }
}

unsigned inotify_bytes(const struct open_file *file) { return file->inotify->bytes; }

static int64_t drain(struct domain *domain, struct inotify_state *state, uint64_t buf, uint64_t len) {
  if (!state->head) { return -11; }
  uint64_t done = 0;
  while (state->head) {
    struct inotify_event_record *record = state->head;
    size_t size = HEADER + record->event.len;
    if (size > len - done) { return done ? (int64_t)done : -22; }
    if (!cell_domain_ensure_user_range(domain, buf + done, size, VMM_ACCESS_WRITE) ||
        !vmm_copy_to_user(cell_domain_as(domain), buf + done, &record->event, size)) {
      return done ? (int64_t)done : -14;
    }
    drop_head(state);
    done += size;
  }
  cell_epoll_file_changed(state->file);
  return (int64_t)done;
}

int64_t inotify_read(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len, struct trap_frame *frame) {
  int64_t n = drain(domain, file->inotify, buf, len);
  if (n != -11 || !frame || (file->flags & NONBLOCK)) { return n; }
  cell_retain_open_file(file);
  return cell_block_current(frame, (struct thread_wait){.reason = WAIT_INOTIFY, .held_file = file, .buf = buf, .len = len});
}

void inotify_flush(void) {
  if (!pending || flushing) { return; }
  flushing = true;
  while (pending) {
    struct inotify_state *state = pending;
    pending = state->pending_next;
    state->pending = false;
    struct open_file *file = state->file;
    cell_epoll_file_activity(file, CELL_POLLIN);
    size_t cursor = 0;
    struct thread *t;
    while (state->head && (t = cell_next_waiter(WAIT_INOTIFY, &cursor))) {
      if (t->wait.held_file == file) {
        int64_t n = drain(t->domain, state, t->wait.buf, t->wait.len);
        cell_finish_wait(t, n);
      }
    }
    cell_release_open_file(file);
  }
  flushing = false;
  cell_wake_poll_waiters_internal();
}
