#include "pl011.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  PL011_PHYS = 0x09000000,
  PL011_DR = 0x00,
  PL011_FR = 0x18,
  PL011_CR = 0x30,
  PL011_IMSC = 0x38,
  PL011_MIS = 0x40,
  PL011_ICR = 0x44,
  PL011_FR_RXFE = 1u << 4,
  PL011_FR_TXFF = 1u << 5,
  PL011_CR_UARTEN = 1u << 0,
  PL011_CR_TXE = 1u << 8,
  PL011_CR_RXE = 1u << 9,
  PL011_INT_RX = 1u << 4,
  PL011_INT_RT = 1u << 6,
  RX_RING_SIZE = 256,
};

static volatile uint32_t *uart_base;
static char rx_ring[RX_RING_SIZE];
static uint16_t rx_head;
static uint16_t rx_tail;
static uint16_t tty_rows = 38;
static uint16_t tty_cols = 96;
static char ctl_buf[32];
static uint8_t ctl_len;
static uint8_t ctl_state;
static uint16_t ctl_rows;
static uint16_t ctl_cols;

static inline uint32_t mmio_read32(uint64_t offset) {
  return *(volatile uint32_t *)((uintptr_t)uart_base + offset);
}

static inline void mmio_write32(uint64_t offset, uint32_t value) {
  *(volatile uint32_t *)((uintptr_t)uart_base + offset) = value;
}

void pl011_init(uint64_t hhdm_offset) {
  uart_base = (volatile uint32_t *)(hhdm_offset + PL011_PHYS);
  rx_head = 0;
  rx_tail = 0;
}

void pl011_putc(char c) {
  if (uart_base == NULL) { return; }
  if (c == '\n') { pl011_putc('\r'); }
  while ((mmio_read32(PL011_FR) & PL011_FR_TXFF) != 0) {
    __asm__ volatile("yield");
  }
  mmio_write32(PL011_DR, (uint32_t)(uint8_t)c);
}

static void rx_push(char c) {
  uint16_t next = (uint16_t)((rx_head + 1u) % RX_RING_SIZE);
  if (next == rx_tail) { return; }
  rx_ring[rx_head] = c;
  rx_head = next;
}

static void ctl_reset(void) {
  ctl_len = 0;
  ctl_state = 0;
  ctl_rows = 0;
  ctl_cols = 0;
}

static void ctl_remember(char c) {
  if (ctl_len < sizeof(ctl_buf)) { ctl_buf[ctl_len++] = c; }
}

static void ctl_flush(void) {
  for (uint8_t i = 0; i < ctl_len; ++i) {
    rx_push(ctl_buf[i]);
  }
  ctl_reset();
}

static void tty_control_char(char c) {
  if (ctl_state == 0) {
    if ((uint8_t)c == 0x1b) {
      ctl_remember(c);
      ctl_state = 1;
      return;
    }
    rx_push(c);
    return;
  }

  ctl_remember(c);
  switch (ctl_state) {
  case 1:
    if (c == ']') {
      ctl_state = 2;
    } else {
      ctl_flush();
    }
    break;
  case 2:
  case 3:
  case 4:
    if (c == '7') {
      ++ctl_state;
    } else {
      ctl_flush();
    }
    break;
  case 5:
    if (c == ';') {
      ctl_state = 6;
    } else {
      ctl_flush();
    }
    break;
  case 6:
    if (c >= '0' && c <= '9') {
      ctl_rows = (uint16_t)(ctl_rows * 10u + (uint16_t)(c - '0'));
    } else if (c == ';') {
      ctl_state = 7;
    } else {
      ctl_flush();
    }
    break;
  case 7:
    if (c >= '0' && c <= '9') {
      ctl_cols = (uint16_t)(ctl_cols * 10u + (uint16_t)(c - '0'));
    } else if ((uint8_t)c == 0x07) {
      if (ctl_rows >= 8 && ctl_cols >= 40) {
        tty_rows = ctl_rows;
        tty_cols = ctl_cols;
      }
      ctl_reset();
    } else {
      ctl_flush();
    }
    break;
  default:
    ctl_flush();
    break;
  }
}

bool pl011_getc(char *out) {
  if (rx_tail == rx_head) { return false; }
  *out = rx_ring[rx_tail];
  rx_tail = (uint16_t)((rx_tail + 1u) % RX_RING_SIZE);
  return true;
}

bool pl011_poll_rx(void) {
  if (uart_base == NULL) { return rx_tail != rx_head; }
  while ((mmio_read32(PL011_FR) & PL011_FR_RXFE) == 0) {
    tty_control_char((char)(mmio_read32(PL011_DR) & 0xffu));
  }
  return rx_tail != rx_head;
}

void pl011_get_winsize(uint16_t *rows, uint16_t *cols) {
  *rows = tty_rows;
  *cols = tty_cols;
}

void pl011_enable_rx_irq(void) {
  if (uart_base == NULL) { return; }
  mmio_write32(PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
  mmio_write32(PL011_ICR, 0x7ff);
  mmio_write32(PL011_IMSC, PL011_INT_RX | PL011_INT_RT);
}

bool pl011_handle_irq(void) {
  if (uart_base == NULL) { return false; }
  uint32_t mis = mmio_read32(PL011_MIS);
  if ((mis & (PL011_INT_RX | PL011_INT_RT)) == 0) { return false; }
  (void)pl011_poll_rx();
  mmio_write32(PL011_ICR, PL011_INT_RX | PL011_INT_RT);
  return true;
}
