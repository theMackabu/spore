#include "arch/clock.h"
#include "arch/platform.h"
#include "arch/smp.h"
#include "drivers/serial.h"
#include "fs/procfs_format.h"
#include "init/app.h"
#include "platform.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/signal.h"
#include "proc/thread.h"
#include "user.h"

void port_timer_interrupt(struct trap_frame *frame) {
  smp_kernel_lock();
  platform_rearm_timer();
  cell_timer_tick(frame, frame != NULL);
  smp_kernel_unlock();
}
void smp_wait_for_irq(void) {
  port_idle_address_space();
  platform_wait_tick();
#ifdef __riscv
  smp_kernel_lock();
  cell_timer_tick(NULL, false);
  smp_kernel_unlock();
#endif
}
void serial_putc(char c) {
  console_putc(c);
}
bool serial_getc(char *c) {
  int value = console_try_getc();
  if (value < 0) { return false; }
  *c = (char)value;
  return true;
}
void serial_get_winsize(uint16_t *rows, uint16_t *cols) {
  *rows = 24;
  *cols = 80;
}
[[noreturn]] void arch_poweroff(void) {
  platform_poweroff();
}
[[noreturn]] void arch_halt(void) {
  platform_poweroff();
}

static void resolve_page_fault(struct trap_frame *f, uint64_t address, enum vmm_access access) {
  bool mapped = vmm_is_mapped(cell_current_as(), address);
  if (!mapped && cell_handle_translation_fault(address, access)) {
    if (access == VMM_ACCESS_EXEC) { app_instruction_fetched(address); }
    return;
  }
  if (mapped && access == VMM_ACCESS_WRITE && cell_handle_write_fault(address)) { return; }
  if (mapped && cell_handle_permission_fault(address, access)) { return; }
  cell_signal_current_fault(11, f, address, mapped ? 2 : 1);
}

size_t proc_cpuinfo_text(char *dst, size_t cap) {
  size_t len = 0;
  for (unsigned cpu = 0; cpu < smp_present_cpu_count(); ++cpu) {
    if (!smp_cpu_online(cpu)) { continue; }
    proc_append_str(dst, cap, &len, "processor\t: ");
    proc_append_u64(dst, cap, &len, cpu);
    proc_append_str(dst, cap, &len, "\nmodel name\t: " SPORE_MACHINE "\n\n");
  }
  return len;
}

void port_page_fault(struct trap_frame *f, uint64_t address, enum vmm_access access) {
  smp_kernel_lock();
  resolve_page_fault(f, address, access);
  smp_kernel_unlock();
}

void port_user_exception(struct trap_frame *frame, int signal, uint64_t address) {
  smp_kernel_lock();
  cell_signal_current_fault(signal, frame, address, 1);
  smp_kernel_unlock();
}
