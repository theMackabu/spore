#include "arch/clock.h"
#include "arch/entropy.h"
#include "arch/regs.h"
#include "lib/mem.h"

uint64_t x86_fsbase(void) {
  uint32_t a, d;
  __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(0xc0000100u));
  return (uint64_t)d << 32 | a;
}
void x86_set_fsbase(uint64_t value) {
  __asm__ volatile("wrmsr" ::"a"((uint32_t)value), "d"((uint32_t)(value >> 32)), "c"(0xc0000100u));
}
void arch_save_context(struct arch_thread_context *c) {
  c->thread_pointer = x86_fsbase();
  __asm__ volatile("fxsave64 %0" : "=m"(c->fp));
}
void arch_restore_context(const struct arch_thread_context *c) {
  x86_set_fsbase(c->thread_pointer);
  __asm__ volatile("fxrstor64 %0" ::"m"(c->fp));
}
void arch_init_user_frame(struct trap_frame *f, uint64_t pc, uint64_t sp, bool bsd) {
  *f = (struct trap_frame){
    .rip = pc, .rsp = bsd ? sp - 8 : sp, .rdi = bsd ? sp : 0, .cs = 0x23, .ss = 0x1b, .rflags = 0x202};
}
void arch_set_snapshot_entry(struct trap_frame *f, uint64_t pc, uint64_t arg, uint64_t pid) {
  f->rip = pc;
  f->rdi = arg;
  f->rsi = pid;
}
bool arch_restart_syscall(struct trap_frame *f) {
  if (f->rip < 2) return false;
  f->rip -= 2;
  return true;
}
uint64_t arch_counter_value(void) {
  uint32_t a, d;
  __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
  return (uint64_t)d << 32 | a;
}
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
  __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
uint64_t arch_counter_frequency(void) {
  static uint64_t frequency;
  if (!frequency) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    uint32_t last_leaf = a;
    if (last_leaf >= 0x15) { cpuid(0x15, &a, &b, &c, &d); }
    if (last_leaf >= 0x15 && a && b && c) {
      frequency = (uint64_t)c * b / a;
    } else {
      uint8_t control;
      __asm__ volatile("inb $0x61,%0" : "=a"(control));
      __asm__ volatile("outb %0,$0x61" ::"a"((uint8_t)(control & ~3u)));
      __asm__ volatile("outb %0,$0x43" ::"a"((uint8_t)0xb0));
      uint16_t n = 11932;
      __asm__ volatile("outb %0,$0x42" ::"a"((uint8_t)n));
      __asm__ volatile("outb %0,$0x42" ::"a"((uint8_t)(n >> 8)));
      uint64_t start = arch_counter_value();
      __asm__ volatile("outb %0,$0x61" ::"a"((uint8_t)((control & ~2u) | 1)));
      uint8_t status;
      do {
        __asm__ volatile("inb $0x61,%0" : "=a"(status));
      } while (!(status & 0x20));
      frequency = (arch_counter_value() - start) * 1193182 / n;
      __asm__ volatile("outb %0,$0x61" ::"a"(control));
    }
  }
  return frequency;
}
bool arch_rndr_supported(void) {
  uint32_t a, b, c, d;
  cpuid(1, &a, &b, &c, &d);
  return (c & (1u << 30)) != 0;
}
bool arch_rndr64(uint64_t *out) {
  unsigned char ok;
  __asm__ volatile("rdrand %0; setc %1" : "=r"(*out), "=qm"(ok)::"cc");
  return ok;
}
