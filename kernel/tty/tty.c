#include "tty/tty.h"

#include "arch/regs.h"
#include "drivers/serial.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "proc/epoll.h"
#include "proc/signal.h"
#include "proc/thread.h"

enum {
  EINTR = 4,
  TTY_OPOST = 0000001,
  TTY_ONLCR = 0000004,
  TTY_ISIG = 0000001,
  TTY_ICANON = 0000002,
  TTY_ECHO = 0000010,
  SIGINT = 2,
};

static uint32_t tty_oflag = TTY_OPOST | TTY_ONLCR;
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
  tty_oflag = TTY_OPOST | TTY_ONLCR;
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

uint32_t cell_tty_oflag(void) {
  return tty_oflag;
}

void cell_tty_set_oflag(uint32_t oflag) {
  tty_oflag = oflag;
}

uint32_t cell_tty_lflag(void) {
  return tty_lflag;
}

uint8_t cell_tty_erase_char(void) {
  return tty_erase;
}

void cell_tty_set_erase_char(uint8_t ch) {
  if (ch != 0) { tty_erase = ch; }
}

int64_t cell_tty_write_console_from_user(struct domain *domain, uint64_t buf, uint64_t len) {
  const struct user_address_space *as = cell_domain_as_const(domain);
  for (uint64_t done = 0; done < len;) {
    uint64_t addr = buf + done;
    if (addr < buf) { return -14; }
    uint64_t pa = vmm_user_to_phys(as, addr);
    if (pa == 0) { return -14; }
    const char *data = (const char *)(uintptr_t)(as->hhdm_offset + pa);
    size_t chunk = PAGE_SIZE - addr % PAGE_SIZE;
    if (chunk > len - done) { chunk = len - done; }
    for (size_t i = 0; i < chunk; ++i) {
      char c = data[i];
      serial_putc(c);
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
    done += chunk;
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
    serial_putc('\n');
    tty_output_line_len = 0;
    tty_output_line[0] = '\0';
  } else if (c == '\b' || c == 0x7f) {
    serial_putc('\b');
    serial_putc(' ');
    serial_putc('\b');
  } else if ((uint8_t)c >= 0x20 || c == '\t') {
    serial_putc(c);
  }
}

static void tty_echo_str(const char *s) {
  if (!tty_echo()) { return; }
  while (*s != '\0') {
    serial_putc(*s++);
  }
}

static void tty_redraw_line(void) {
  tty_echo_str("\r\033[K");
  for (size_t i = 0; i < tty_prompt_len; ++i) {
    serial_putc(tty_prompt[i]);
  }
  for (size_t i = 0; i < tty_line_len; ++i) {
    serial_putc(tty_line[i]);
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
      serial_putc(rev[n - 1]);
      --n;
    }
    serial_putc('D');
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

void cell_tty_flush_input(void) {
  tty_clear_pending_input();
  serial_flush_input();
  cell_epoll_console_activity(0);
}

void cell_tty_set_lflag(uint32_t lflag, bool flush_input) {
  if (flush_input) {
    cell_tty_flush_input();
  } else if (tty_canonical() && !(lflag & TTY_ICANON)) {
    for (size_t i = 0; i < tty_line_len; ++i) { tty_ready_push(tty_line[i]); }
    tty_line_len = 0;
    tty_line_cursor = 0;
    tty_prompt_active = false;
  }
  tty_lflag = lflag;
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
  if (!serial_getc(&bracket)) { return true; }
  if (bracket != '[') { return true; }

  char final = '\0';
  while (serial_getc(&final)) {
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

static void buffer_input(bool read_ahead) {
  char c;
  while ((read_ahead || tty_ready_len == 0) && tty_ready_len + tty_line_len < sizeof(tty_ready) && serial_getc(&c)) {
    if (tty_isig() && c == 3) {
      if (tty_echo()) {
        serial_putc('^');
        serial_putc('C');
        serial_putc('\n');
      }
      tty_clear_pending_input();
      tty_pending_signal = SIGINT;
      break;
    }
    if (!tty_canonical()) {
      if ((uint8_t)c == 0x08 || (uint8_t)c == 0x7f) { c = (char)tty_erase; }
      tty_ready_push(c);
      continue;
    }
    if (c == 0x1b) {
      (void)tty_try_escape();
      continue;
    }
    if (c == '\r') { c = '\n'; }
    if (c == '\n') {
      tty_echo_char('\n');
      tty_line_commit();
      continue;
    }
    if (c == 3) {
      if (tty_echo()) {
        serial_putc('^');
        serial_putc('C');
        serial_putc('\n');
      }
      tty_clear_pending_input();
      tty_ready_push('\n');
      break;
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

unsigned cell_tty_read_available(void) {
  buffer_input(true);
  return (unsigned)tty_ready_len;
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
      buffer_input(false);
      if (tty_pending_signal != 0) { return -EINTR; }
      if (!tty_ready_pop(&c)) { break; }
    }
    if (!vmm_copy_to_user(cell_domain_as(domain), buf + n, &c, 1)) { return -14; }
    ++n;
    if (tty_canonical() && c == '\n') { break; }
  }
  if (n && !tty_ready_len) { cell_epoll_console_activity(0); }
  return (int64_t)n;
}

bool cell_tty_stdin_readable(void) {
  if (tty_ready_len != 0 || tty_pending_signal != 0) { return true; }
  buffer_input(false);
  return tty_ready_len != 0 || tty_pending_signal != 0;
}

static void tty_deliver_pending_signal_to_foreground(struct trap_frame *frame) {
  int signal = cell_tty_take_pending_signal();
  if (signal == 0) { return; }
  int pgrp = cell_tty_foreground_pgrp();
  if (pgrp <= 0) { return; }
  for (size_t i = 0, limit = cell_domain_slot_limit(); i < limit; ++i) {
    struct domain *domain = cell_domain_slot(i);
    if (domain == NULL || !domain->used || domain->zombie || domain->pgrp_id != pgrp) { continue; }
    cell_signal_domain(domain, signal, frame);
  }
}

void cell_wake_stdin(struct trap_frame *frame) {
  size_t previous = tty_ready_len;
  buffer_input(true);
  if (tty_ready_len > previous) { cell_epoll_console_activity(CELL_POLLIN); }
  tty_deliver_pending_signal_to_foreground(frame);
  cell_wake_poll_waiters_internal();
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_STDIN, &cursor))) {
    int64_t n = cell_tty_read_to_user(thread->domain, thread->wait.buf, thread->wait.len);
    if (n == -EINTR && cell_tty_pending_signal() != 0) {
      int signal = cell_tty_take_pending_signal();
      (void)cell_deliver_signal_to_thread(thread, signal);
      continue;
    }
    if (n <= 0) { continue; }
    cell_finish_wait(thread, n);
  }
  cell_wake_poll_waiters_internal();
}
