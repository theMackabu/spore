#include "arch/aarch64/exceptions.h"

#include "arch/aarch64/regs.h"
#include "kprintf.h"

extern char vectors[];

void exceptions_init(void) {
  uint64_t cpacr;
  __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
  cpacr |= 3ull << 20;
  __asm__ volatile("msr cpacr_el1, %0\n"
                   "isb\n"
                   "msr vbar_el1, %1\n"
                   "isb\n"
                   :
                   : "r"(cpacr), "r"(vectors)
                   : "memory");
}

void handle_unhandled_exception(void) {
  uint64_t esr;
  uint64_t elr;
  uint64_t far;
  __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
  __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
  __asm__ volatile("mrs %0, far_el1" : "=r"(far));
  kprintf("[kernel] unhandled exception esr=%x elr=%p far=%p\n", (unsigned)esr, (void *)(uintptr_t)elr,
          (void *)(uintptr_t)far);
}

void handle_lower_sync(struct trap_frame *frame);
