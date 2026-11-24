#include "arch/aarch64/exceptions.h"

#include "arch/aarch64/regs.h"
#include "lib/kprintf.h"
#include "init/app.h"

#include "abi/linux/aarch64/syscall.h"
#include "abi/freebsd/aarch64.h"
#include "arch/signal_frame.h"
#include "proc/memory.h"
#include "proc/signal.h"

enum {
  EC_UNKNOWN = 0x00,
  EC_SVC_A64 = 0x15,
  EC_SYS64 = 0x18,
  EC_SVE = 0x19,
  EC_SME = 0x1d,
  SIGILL = 4,
  SIGSEGV = 11,
  ILL_ILLOPC = 1,
  SEGV_MAPERR = 1,
  SEGV_ACCERR = 2
};

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

void handle_lower_sync(struct trap_frame *frame) {
  uint64_t ec = (frame->esr_el1 >> 26) & 0x3f;
  if (ec == EC_UNKNOWN || ec == EC_SYS64 || ec == EC_SVE || ec == EC_SME) {
    cell_signal_current_fault(SIGILL, frame, frame->elr_el1, ILL_ILLOPC);
    return;
  }
  if (ec != EC_SVC_A64) {
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    uint64_t iss = frame->esr_el1 & 0x1ffffff;
    uint64_t dfsc = iss & 0x3f;
    bool write = (iss & (1u << 6)) != 0;
    if (ec == 0x20 && dfsc >= 0x04 && dfsc <= 0x07 && cell_handle_translation_fault(far, VMM_ACCESS_EXEC)) {
      app_instruction_fetched(frame->elr_el1);
      return;
    }
    if (ec == 0x20 && dfsc >= 0x0c && dfsc <= 0x0f && cell_handle_permission_fault(far, VMM_ACCESS_EXEC)) { return; }
    if (ec == 0x24 && dfsc >= 0x04 && dfsc <= 0x07 &&
        cell_handle_translation_fault(far, write ? VMM_ACCESS_WRITE : VMM_ACCESS_READ)) {
      return;
    }
    if (ec == 0x24 && write && dfsc >= 0x0c && dfsc <= 0x0f && cell_handle_write_fault(far)) { return; }
    if (ec == 0x24 && dfsc >= 0x0c && dfsc <= 0x0f &&
        cell_handle_permission_fault(far, write ? VMM_ACCESS_WRITE : VMM_ACCESS_READ)) {
      return;
    }
    kprintf("[kernel] lower sync fault ec=%x dfsc=%x write=%u esr=%x elr=%p far=%p\n", (unsigned)ec, (unsigned)dfsc,
            write ? 1u : 0u, (unsigned)frame->esr_el1, (void *)(uintptr_t)frame->elr_el1, (void *)(uintptr_t)far);
    cell_dump_current_fault(frame, far);
    bool translation_fault = (dfsc >= 0x04 && dfsc <= 0x07);
    cell_signal_current_fault(SIGSEGV, frame, far, translation_fault ? SEGV_MAPERR : SEGV_ACCERR);
    return;
  }
  if (cell_current_as()->freebsd_abi) { freebsd_aarch64_syscall(frame); }
  else { linux_aarch64_syscall(frame); }
}
