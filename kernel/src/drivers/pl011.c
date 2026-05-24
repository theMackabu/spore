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

bool pl011_getc(char *out) {
  if (rx_tail == rx_head) { return false; }
  *out = rx_ring[rx_tail];
  rx_tail = (uint16_t)((rx_tail + 1u) % RX_RING_SIZE);
  return true;
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
  while ((mmio_read32(PL011_FR) & PL011_FR_RXFE) == 0) {
    rx_push((char)(mmio_read32(PL011_DR) & 0xffu));
  }
  mmio_write32(PL011_ICR, PL011_INT_RX | PL011_INT_RT);
  return true;
}
