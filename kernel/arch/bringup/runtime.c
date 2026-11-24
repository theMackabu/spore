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

static struct thread *current;
static bool waiting;

uint32_t smp_current_cpu(void) {
  return 0;
}
uint32_t smp_possible_cpu_count(void) {
  return 1;
}
uint32_t smp_present_cpu_count(void) {
  return 1;
}
uint32_t smp_online_cpu_count(void) {
  return 1;
}
bool smp_cpu_present(uint32_t n) {
  return n == 0;
}
bool smp_cpu_online(uint32_t n) {
  return n == 0;
}
void *smp_current_thread_slot(uint32_t cpu) {
  return cpu == 0 ? current : NULL;
}
void smp_set_current_thread_slot(uint32_t cpu, void *thread) {
  if (!cpu) { current = thread; }
}
void smp_clear_current_thread_if(uint32_t cpu, void *thread) {
  if (!cpu && current == thread) { current = NULL; }
}
bool smp_scheduler_waiting(uint32_t cpu) {
  return !cpu && waiting;
}
void smp_set_scheduler_waiting(uint32_t cpu, bool value) {
  if (!cpu) { waiting = value; }
}
void smp_notify(void) {}
void smp_kernel_lock(void) {}
void smp_kernel_unlock(void) {}
void smp_start_scheduler_cpus(void) {}

void port_timer_interrupt(struct trap_frame *frame) {
  platform_rearm_timer();
  cell_timer_tick(frame, frame != NULL);
}
void smp_wait_for_irq(void) {
  platform_wait_tick();
#ifdef __riscv
  cell_timer_tick(NULL, false);
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

void port_page_fault(struct trap_frame *f, uint64_t address, enum vmm_access access) {
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
  proc_append_str(dst, cap, &len, "processor\t: 0\nmodel name\t: " SPORE_MACHINE "\n");
  return len;
}
