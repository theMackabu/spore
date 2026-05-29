#include "proc/tty.h"

#include "pl011.h"
#include "proc/domain.h"
#include "proc/poll.h"
#include "proc/signal.h"
#include "proc/thread.h"

enum {
  EINTR = 4,
  TTY_ISIG = 0000001,
  TTY_ICANON = 0000002,
  TTY_ECHO = 0000010,
  SIGINT = 2,
};

static uint32_t tty_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO;
static uint8_t tty_erase = 0x7f;
static char tty_line[8192];
static size_t tty_line_len;
static size_t tty_line_cursor;
static char tty_ready[8192];
static size_t tty_ready_head;
static size_t tty_ready_len;
static int tty_pending_signal;
static char tty_output_line[256];
static size_t tty_output_line_len;
static char tty_prompt[256];
static size_t tty_prompt_len;
static bool tty_prompt_active;
static int tty_foreground_pgrp;

void cell_tty_reset(void) {
  tty_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO;
  tty_erase = 0x7f;
  tty_line_len = 0;
  tty_line_cursor = 0;
  tty_ready_head = 0;
  tty_ready_len = 0;
  tty_pending_signal = 0;
  tty_output_line_len = 0;
  tty_output_line[0] = '\0';
  tty_prompt_len = 0;
  tty_prompt[0] = '\0';
  tty_prompt_active = false;
  tty_foreground_pgrp = 0;
}

int cell_tty_foreground_pgrp(void) {
  struct domain *domain = cell_current_domain_internal();
  if (tty_foreground_pgrp == 0 && domain != NULL) { tty_foreground_pgrp = domain->pgrp_id; }
  return tty_foreground_pgrp;
}

int cell_tty_set_foreground_pgrp(int pgid) {
  if (pgid <= 0) { return -22; }
  tty_foreground_pgrp = pgid;
  return 0;
}

static bool tty_canonical(void) {
  return (tty_lflag & TTY_ICANON) != 0;
}

static bool tty_isig(void) {
  return (tty_lflag & TTY_ISIG) != 0;
}

static bool tty_echo(void) {
  return (tty_lflag & TTY_ECHO) != 0;
}

uint32_t cell_tty_lflag(void) {
  return tty_lflag;
}

void cell_tty_set_lflag(uint32_t lflag) {
  tty_lflag = lflag;
  if (!tty_canonical()) {
    tty_line_len = 0;
    tty_line_cursor = 0;
    tty_ready_head = 0;
    tty_ready_len = 0;
    tty_prompt_active = false;
  }
}

uint8_t cell_tty_erase_char(void) {
  return tty_erase;
}

void cell_tty_set_erase_char(uint8_t ch) {
  if (ch != 0) { tty_erase = ch; }
}

int64_t cell_tty_write_console_from_user(struct domain *domain, uint64_t buf, uint64_t len) {
  for (uint64_t i = 0; i < len; ++i) {
    char c;
    if (!vmm_copy_from_user(cell_domain_as(domain), &c, buf + i, 1)) { return -14; }
    pl011_putc(c);
    if (c == '\r' || c == '\n') {
      tty_output_line_len = 0;
      tty_output_line[0] = '\0';
    } else if ((uint8_t)c >= 0x20 || c == '\t' || c == '\033') {
      if (tty_output_line_len + 1 < sizeof(tty_output_line)) {
        tty_output_line[tty_output_line_len++] = c;
        tty_output_line[tty_output_line_len] = '\0';
      }
    }
  }
  return (int64_t)len;
}

static void tty_begin_canonical_read(void) {
  if (tty_prompt_active) { return; }
  tty_prompt_len = tty_output_line_len;
  if (tty_prompt_len >= sizeof(tty_prompt)) { tty_prompt_len = sizeof(tty_prompt) - 1; }
  for (size_t i = 0; i < tty_prompt_len; ++i) {
    tty_prompt[i] = tty_output_line[i];
  }
  tty_prompt[tty_prompt_len] = '\0';
  tty_line_cursor = tty_line_len;
  tty_prompt_active = true;
}

static void tty_echo_char(char c) {
  if (!tty_echo()) { return; }
  if (c == '\n') {
    pl011_putc('\n');
    tty_output_line_len = 0;
    tty_output_line[0] = '\0';
  } else if (c == '\b' || c == 0x7f) {
    pl011_putc('\b');
    pl011_putc(' ');
    pl011_putc('\b');
  } else if ((uint8_t)c >= 0x20 || c == '\t') {
    pl011_putc(c);
  }
}

static void tty_echo_str(const char *s) {
  if (!tty_echo()) { return; }
  while (*s != '\0') {
    pl011_putc(*s++);
  }
}

static void tty_redraw_line(void) {
  tty_echo_str("\r\033[K");
  for (size_t i = 0; i < tty_prompt_len; ++i) {
    pl011_putc(tty_prompt[i]);
  }
  for (size_t i = 0; i < tty_line_len; ++i) {
    pl011_putc(tty_line[i]);
  }
  if (tty_line_cursor < tty_line_len) {
    tty_echo_str("\033[");
    size_t move = tty_line_len - tty_line_cursor;
    char rev[20];
    size_t n = 0;
    do {
      rev[n++] = (char)('0' + (move % 10u));
      move /= 10u;
    } while (move != 0 && n < sizeof(rev));
    while (n > 0) {
      pl011_putc(rev[n - 1]);
      --n;
    }
    pl011_putc('D');
  }
}

static void tty_ready_push(char c) {
  if (tty_ready_len >= sizeof(tty_ready)) { return; }
  size_t index = (tty_ready_head + tty_ready_len) % sizeof(tty_ready);
  tty_ready[index] = c;
  ++tty_ready_len;
}

static bool tty_ready_pop(char *out) {
  if (tty_ready_len == 0) { return false; }
  *out = tty_ready[tty_ready_head];
  tty_ready_head = (tty_ready_head + 1u) % sizeof(tty_ready);
  --tty_ready_len;
  return true;
}

static void tty_clear_pending_input(void) {
  tty_line_len = 0;
  tty_line_cursor = 0;
  tty_ready_head = 0;
  tty_ready_len = 0;
  tty_prompt_active = false;
}

static void tty_line_commit(void) {
  for (size_t i = 0; i < tty_line_len; ++i) {
    tty_ready_push(tty_line[i]);
  }
  tty_ready_push('\n');
  tty_line_len = 0;
  tty_line_cursor = 0;
  tty_prompt_active = false;
}

static bool tty_try_escape(void) {
  char bracket;
  if (!pl011_getc(&bracket)) { return true; }
  if (bracket != '[') { return true; }

  char final = '\0';
  while (pl011_getc(&final)) {
    if (final >= '@' && final <= '~') { break; }
  }

  if (final == 'C') {
    if (tty_line_cursor < tty_line_len) {
      ++tty_line_cursor;
      tty_echo_str("\033[C");
    }
  } else if (final == 'D') {
    if (tty_line_cursor > 0) {
      --tty_line_cursor;
      tty_echo_str("\033[D");
    }
  }
  return true;
}

void cell_tty_process_input(void) {
  char c;
  while (tty_ready_len == 0 && pl011_getc(&c)) {
    if (tty_isig() && c == 3) {
      if (tty_echo()) {
        pl011_putc('^');
        pl011_putc('C');
        pl011_putc('\n');
      }
      tty_clear_pending_input();
      tty_pending_signal = SIGINT;
      return;
    }
    if (!tty_canonical()) {
      if ((uint8_t)c == 0x08 || (uint8_t)c == 0x7f) { c = (char)tty_erase; }
      tty_ready_push(c);
      return;
    }
    if (c == 0x1b) {
      (void)tty_try_escape();
      continue;
    }
    if (c == '\r') { c = '\n'; }
    if (c == '\n') {
      tty_echo_char('\n');
      tty_line_commit();
      return;
    }
    if (c == 3) {
      if (tty_echo()) {
        pl011_putc('^');
        pl011_putc('C');
        pl011_putc('\n');
      }
      tty_clear_pending_input();
      tty_ready_push('\n');
      return;
    }
    if ((uint8_t)c == 0x08 || (uint8_t)c == 0x7f || (uint8_t)c == tty_erase) {
      if (tty_line_cursor > 0) {
        for (size_t i = tty_line_cursor - 1; i + 1 < tty_line_len; ++i) {
          tty_line[i] = tty_line[i + 1];
        }
        --tty_line_len;
        --tty_line_cursor;
        tty_redraw_line();
      }
      continue;
    }
    if ((uint8_t)c < 0x20 && c != '\t') { continue; }
    if (tty_line_len + 1 < sizeof(tty_line)) {
      for (size_t i = tty_line_len; i > tty_line_cursor; --i) {
        tty_line[i] = tty_line[i - 1];
      }
      tty_line[tty_line_cursor++] = c;
      ++tty_line_len;
      if (tty_line_cursor == tty_line_len) {
        tty_echo_char(c);
      } else {
        tty_redraw_line();
      }
    }
  }
}

int cell_tty_pending_signal(void) {
  return tty_pending_signal;
}

int cell_tty_take_pending_signal(void) {
  int signal = tty_pending_signal;
  tty_pending_signal = 0;
  return signal;
}

int64_t cell_tty_read_to_user(struct domain *domain, uint64_t buf, uint64_t len) {
  if (tty_canonical()) { tty_begin_canonical_read(); }
  if (tty_pending_signal != 0) { return -EINTR; }
  uint64_t n = 0;
  while (n < len) {
    char c;
    if (!tty_ready_pop(&c)) {
      cell_tty_process_input();
      if (tty_pending_signal != 0) { return -EINTR; }
      if (!tty_ready_pop(&c)) { break; }
    }
    if (!vmm_copy_to_user(cell_domain_as(domain), buf + n, &c, 1)) { return -14; }
    ++n;
    if (tty_canonical() && c == '\n') { break; }
  }
  return (int64_t)n;
}

bool cell_tty_stdin_readable(void) {
  if (tty_ready_len != 0 || tty_pending_signal != 0) { return true; }
  cell_tty_process_input();
  return tty_ready_len != 0 || tty_pending_signal != 0;
}

static struct thread *tty_signal_target_for_domain(struct domain *domain) {
  struct thread *fallback = NULL;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state == THREAD_UNUSED || thread->domain != domain) { continue; }
    if (fallback == NULL) { fallback = thread; }
    if (thread->state == THREAD_BLOCKED && thread->wait_reason == WAIT_STDIN) { return thread; }
  }
  return fallback;
}

static void tty_deliver_pending_signal_to_foreground(struct trap_frame *frame) {
  int signal = cell_tty_take_pending_signal();
  if (signal == 0) { return; }
  int pgrp = cell_tty_foreground_pgrp();
  if (pgrp <= 0) { return; }
  struct thread *interrupted = cell_current_thread_internal();
  for (size_t i = 0; i < MAX_DOMAINS; ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used || domain->zombie || domain->pgrp_id != pgrp) { continue; }
    struct thread *thread = tty_signal_target_for_domain(domain);
    if (thread == interrupted && frame != NULL) {
      cell_signal_current(signal, frame);
    } else {
      (void)cell_deliver_signal_to_thread(thread, signal);
    }
  }
}

void cell_wake_stdin(struct trap_frame *frame) {
  cell_tty_process_input();
  tty_deliver_pending_signal_to_foreground(frame);
  cell_wake_poll_waiters_internal();
  cell_wake_epoll_waiters_internal();
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_STDIN) { continue; }
    int64_t n = cell_tty_read_to_user(thread->domain, thread->stdin_buf, thread->stdin_len);
    if (n == -EINTR && cell_tty_pending_signal() != 0) {
      int signal = cell_tty_take_pending_signal();
      (void)cell_deliver_signal_to_thread(thread, signal);
      continue;
    }
    if (n <= 0) { continue; }
    thread->tf.x[0] = (uint64_t)n;
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    thread->stdin_buf = 0;
    thread->stdin_len = 0;
  }
  cell_wake_poll_waiters_internal();
}
