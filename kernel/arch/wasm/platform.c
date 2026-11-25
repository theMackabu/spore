#include "arch/platform.h"
#include "abi/freebsd/aarch64.h"
#include "abi/linux/aarch64/syscall.h"
#include "abi/linux/shared/numbers.h"
#include "abi/linux/shared/usercopy.h"
#include "arch/aarch64/mmu.h"
#include "arch/clock.h"
#include "arch/entropy.h"
#include "arch/smp.h"
#include "drivers/serial.h"
#include "drivers/virtio_blk.h"
#include "drivers/virtio_console.h"
#include "drivers/virtio_net.h"
#include "fs/ext2.h"
#include "fs/procfs_format.h"
#include "fs/ramfs.h"
#include "init/app.h"
#include "init/boot.h"
#include "init/process.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include "lib/random.h"
#include "mm/pmm.h"
#include "net/net.h"
#include "proc/accounting.h"
#include "proc/bootstrap.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/poll.h"
#include "proc/signal.h"
#include "proc/thread.h"
#include "tty/tty.h"

void wasm_open_input(struct domain *domain);
void wasm_poll_input(void);

#define HOST(name) __attribute__((import_module("spore"), import_name(name)))
HOST("clock") uint64_t host_clock(void);
HOST("random") void host_random(void *, size_t);
HOST("net_recv") int host_net_recv(void *, size_t);
HOST("net_send") int host_net_send(const void *, size_t);
HOST("disk_flush") int host_disk_flush(void);
HOST("disk_write") int host_disk_write(uint64_t, const void *, size_t);
HOST("exception") void host_exception(uint64_t, const char *, size_t);
HOST("winsize") uint32_t host_winsize(void);
HOST("serial_read") int host_serial_read(void);
HOST("boot_read") int host_boot_read(uint64_t, void *, size_t);
HOST("entry") void host_entry(void);
HOST("disk_read") int host_disk_read(uint64_t, void *, size_t);
HOST("kernel_log") void host_kernel_log(const void *, size_t);
HOST("write") void host_write(unsigned, const void *, size_t);
HOST("finish") [[noreturn]] void host_finish(int, unsigned, uint64_t, const char *, size_t);

static struct {
  struct thread *thread;
  bool idle;
} cpus[SPORE_BOOT_CPU_MAX];
static uint32_t cpu_id, cpu_count;
static uint8_t output[4096];
static uint8_t log_output[512];
static size_t log_buffered;
static unsigned channel = 1, buffered;
static struct arch_thread_context *active_context;
static uint64_t fault_pc;
static const char *fault_text;
static size_t fault_len;

static void flush_kernel_log(void) {
  if (log_buffered) {
    host_kernel_log(log_output, log_buffered);
    log_buffered = 0;
  }
}

static void wasm_flush(void) {
  flush_kernel_log();
  if (buffered) {
    host_write(channel, output, buffered);
    buffered = 0;
  }
}

void serial_putc(char c) {
  output[buffered++] = c;
  if (buffered == sizeof(output)) { wasm_flush(); }
}
bool serial_getc(char *out) {
  int byte = host_serial_read();
  if (byte < 0) { return false; }
  *out = (char)byte;
  return true;
}
void serial_flush_input(void) {
  while (host_serial_read() >= 0) {}
}
static uint32_t dimensions;
void serial_get_winsize(uint16_t *rows, uint16_t *cols) {
  *rows = dimensions >> 16;
  *cols = (uint16_t)dimensions;
}

uint64_t arch_counter_value(void) {
  return host_clock();
}
uint64_t arch_counter_frequency(void) {
  return 1000000;
}
bool arch_rndr_supported(void) {
  return true;
}
bool arch_rndr64(uint64_t *out) {
  host_random(out, sizeof(*out));
  return true;
}

void wasm_select_cpu(uint32_t cpu) {
  cpu_id = cpu;
}
uint32_t smp_present_cpu_count(void) {
  return cpu_count;
}
uint32_t smp_possible_cpu_count(void) {
  return cpu_count;
}
uint32_t smp_online_cpu_count(void) {
  return cpu_count;
}
uint32_t smp_current_cpu(void) {
  return cpu_id;
}
bool smp_cpu_present(uint32_t cpu) {
  return cpu < cpu_count;
}
bool smp_cpu_online(uint32_t cpu) {
  return cpu < cpu_count;
}
uint64_t smp_cpu_hardware_id(uint32_t cpu) {
  return cpu;
}
void *smp_current_thread_slot(uint32_t cpu) {
  return cpu < cpu_count ? cpus[cpu].thread : NULL;
}
void smp_set_current_thread_slot(uint32_t cpu, void *t) {
  if (cpu < cpu_count) { cpus[cpu].thread = t; }
}
void smp_clear_current_thread_if(uint32_t cpu, void *t) {
  if (cpu < cpu_count && cpus[cpu].thread == t) { cpus[cpu].thread = NULL; }
}
bool smp_scheduler_waiting(uint32_t cpu) {
  return cpu < cpu_count && cpus[cpu].idle;
}
void smp_set_scheduler_waiting(uint32_t cpu, bool waiting) {
  if (cpu < cpu_count) { cpus[cpu].idle = waiting; }
}
void smp_kernel_lock(void) {}
void smp_kernel_unlock(void) {}
void smp_notify(void) {}

void aarch64_publish_table(uint64_t *slot, uint64_t entry) {
  *slot = entry;
}
uint64_t wasm_tlb_generation;
void aarch64_tlb_flush_va(uint64_t va) {
  ++wasm_tlb_generation;
  (void)va;
}
void aarch64_tlb_flush_pages(uint64_t va, uint64_t mask) {
  ++wasm_tlb_generation;
  (void)va;
  (void)mask;
}
void aarch64_tlb_flush_all(void) {
  ++wasm_tlb_generation;
}
void vmm_install_user(const struct user_address_space *as) {
  ++wasm_tlb_generation;
  (void)as;
}

void arch_save_context(struct arch_thread_context *out) {
  *out = *active_context;
}
void arch_restore_context(const struct arch_thread_context *in) {
  *active_context = *in;
}

static bool log_console;
bool virtio_console_ready(void) {
  return log_console;
}
void virtio_console_write(const uint8_t *p, size_t n) {
  if (!log_console) { return; }
  for (size_t i = 0; i < n; ++i) {
    log_output[log_buffered++] = p[i];
    if (p[i] == '\n' || log_buffered == sizeof(log_output)) { flush_kernel_log(); }
  }
}
static bool has_net;
static struct virtio_net_stats net_stats;
bool virtio_net_send_frame(const void *frame, uint32_t len) {
  if (!has_net || !host_net_send(frame, len)) { return false; }
  net_stats.tx_bytes += len;
  ++net_stats.tx_packets;
  return true;
}
void virtio_net_poll(void) {
  if (!has_net) { return; }
  uint8_t frame[1518];
  for (unsigned i = 0; i < 64; ++i) {
    int len = host_net_recv(frame, sizeof(frame));
    if (len <= 0 || (size_t)len > sizeof(frame)) { break; }
    ++net_stats.rx_packets;
    net_stats.rx_bytes += len;
    net_receive_ethernet(frame, len);
  }
}
struct virtio_net_stats virtio_net_stats(void) {
  return net_stats;
}
bool virtio_blk_read(uint64_t off, void *dst, uint32_t n) {
  return host_disk_read(off, dst, n);
}
bool virtio_blk_write(uint64_t off, const void *p, uint32_t n) {
  return host_disk_write(off, p, n);
}
bool virtio_blk_read_boot(uint64_t off, void *p, uint32_t n) {
  return host_boot_read(off, p, n);
}
static bool disk_read(void *ctx, uint64_t off, void *p, uint32_t n) {
  (void)ctx;
  return virtio_blk_read(off, p, n);
}

static bool disk_write(void *ctx, uint64_t off, const void *p, uint32_t n) {
  (void)ctx;
  return virtio_blk_write(off, p, n);
}

static bool disk_flush(void *ctx) {
  (void)ctx;
  return host_disk_flush() != 0;
}

bool wasm_shutdown_storage(void) {
  return vfs_shutdown();
}

void app_finish(enum app_result kind, unsigned value) {
  if (!wasm_shutdown_storage()) {
    kind = APP_BOOT_ERROR;
    value = BOOT_FLUSH_FAILED;
  }
  wasm_flush();
  host_finish(kind, value, fault_pc, fault_text, fault_len);
}
void boot_fail(enum boot_error error, const char *why) {
  host_write(2, why, kstrlen(why));
  host_write(2, "\n", 1);
  app_finish(APP_BOOT_ERROR, error);
}
void arch_halt(void) {
  boot_fail(BOOT_INIT_FAILED, "kernel halted");
}
void arch_poweroff(void) {
  struct domain *init = cell_find_domain(1);
  if (init && init->zombie) {
    if (init->term_signal) { app_finish(APP_SIGNAL, init->term_signal); }
    app_finish(APP_EXIT, init->exit_status);
  }
  app_finish(APP_EXIT, 0);
}
void arch_reboot(void) {
  if (!wasm_shutdown_storage()) { boot_fail(BOOT_FLUSH_FAILED, "cannot flush before reboot"); }
  wasm_flush();
  host_finish('R', 0, 0, NULL, 0);
}

struct trap_frame *wasm_boot(const uint8_t *image, size_t size, const char *args, size_t bytes, unsigned devices,
                             uint64_t epoch, void *ram, size_t ram_bytes, unsigned count) {
  log_console = (devices & 64) != 0;
  dimensions = host_winsize();
  cpu_count = count;
  for (unsigned i = 1; i < count; ++i) {
    cpus[i].idle = true;
  }
  const uint64_t hhdm = (uintptr_t)ram;
  struct spore_boot_module payload = {
    .phys_addr = (uint64_t)(uintptr_t)image - hhdm, .size = size, .path = "/spore.app"};
  struct spore_boot_info boot = {.hhdm_offset = hhdm, .modules = size ? &payload : NULL, .module_count = size ? 1 : 0};
  struct boot_profile profile = {0};
  boot_select_profile(&boot, &profile);
  const struct spore_memmap_entry usable = {
    .base = 0x100000, .length = ram_bytes - 0x100000, .type = SPORE_MEMMAP_USABLE};
  pmm_init(hhdm, &usable, 1);
  klog_handoff();
  syscall_set_boot_time(epoch);
  cell_set_boot_epoch(epoch);
  random_init(epoch);
  has_net = (devices & 8) != 0;
  if (has_net) { net_init(); }
  cell_system_init();
  static struct ext2_fs volume;
  struct ext2_fs *disk = NULL;
  if (devices & 1) {
    if (!ext2_mount_rw(&volume, disk_read, (devices & 2) ? disk_write : NULL, NULL)) {
      boot_fail(BOOT_DATA_MOUNT_FAILED, "cannot mount ext2 disk");
    }
    volume.flush_device = disk_flush;
    disk = &volume;
  }
  if (!size || (size >= 8 && !kmemcmp(image, "SPOREAP2", 8))) {
    if (bytes || (!size && !disk)) { boot_fail(BOOT_INVALID_PAYLOAD, "missing root disk or invalid arguments"); }
    boot_mount_root(&boot, disk, &profile);
  } else {
    static struct ramfs root;
    if (!ramfs_init(&root, NULL, 0, hhdm)) { boot_fail(BOOT_INIT_FAILED, "cannot initialize ramfs"); }
    struct init_program program = {.path = "/app/server", .cwd = "/"};
    if (!ramfs_add_boot_file(&root, program.path, image, size, 0555)) {
      boot_fail(BOOT_INIT_FAILED, "cannot stage ELF");
    }
    while (bytes) {
      size_t len = 0;
      while (len < bytes && args[len]) {
        ++len;
      }
      if (len == bytes || program.argc == INIT_MAX_ARGS) { boot_fail(BOOT_INVALID_PAYLOAD, "invalid app arguments"); }
      program.argv[program.argc++] = args;
      args += len + 1;
      bytes -= len + 1;
    }
    if (!program.argc) { boot_fail(BOOT_INVALID_PAYLOAD, "missing argv[0]"); }
    if (disk && !ramfs_mkdir(&root, 0, "/data", 0555)) { boot_fail(BOOT_DATA_MOUNT_FAILED, "cannot create /data"); }
    vfs_init(&root, disk);
    profile.program = program;
  }
  struct first_process first;
  boot_load_process(&first, &profile.program, hhdm);
  boot_activate_process(&first);
  syscall_set_address_space(cell_current_as());
  if (devices & 32) {
    wasm_open_input(cpus[0].thread->domain);
    cell_tty_set_oflag(0);
    wasm_poll_input();
  } else if (!(devices & 4)) {
    struct vfs_node null;
    if (!vfs_lookup("/dev/null", &null) || cell_fd_close(0) ||
        cell_fd_open_node(&null, 0, "/dev/null", "/dev/null", NULL) != 0) {
      boot_fail(BOOT_INIT_FAILED, "cannot initialize stdin");
    }
    cell_tty_set_oflag(0);
  }
  return &cpus[0].thread->tf;
}

void *wasm_lookup_page(uint64_t page, enum vmm_access access) {
  struct user_address_space *as = cell_current_as();
  if (page >= USER_VA_LIMIT || !vmm_user_range_accessible(as, page, 1, access)) { return NULL; }
  uint64_t pa = vmm_user_to_phys(as, page);
  return pa ? pmm_phys_to_virt(pa) : NULL;
}

static bool entered;
void *wasm_translate(uint64_t page, enum vmm_access access) {
  struct user_address_space *as = cell_current_as();
  if (page >= USER_VA_LIMIT || !cell_domain_ensure_user_range(cpus[cpu_id].thread->domain, page, 1, access)) {
    return NULL;
  }
  if (!vmm_user_range_accessible(as, page, 1, access)) {
    if (access != VMM_ACCESS_WRITE || !cell_handle_write_fault(page)) { return NULL; }
  }
  uint64_t pa = vmm_user_to_phys(as, page);
  if (pa && access == VMM_ACCESS_EXEC && !entered) {
    entered = true;
    host_entry();
  }
  return pa ? pmm_phys_to_virt(pa) : NULL;
}

bool wasm_syscall(struct trap_frame *f, struct arch_thread_context *context) {
  active_context = context;
  uint64_t nr = f->x[8];
  bool bsd = cell_current_as()->freebsd_abi;
  bool write = bsd ? (nr == 4 || nr == 121) : (nr == SYS_WRITE || nr == SYS_WRITEV);
  channel = write && f->x[0] == 2 ? 2 : 1;
  if (bsd) {
    freebsd_aarch64_syscall(f);
  } else {
    linux_aarch64_syscall(f);
  }
  wasm_flush();
  active_context = NULL;
  return cpus[cpu_id].idle;
}

bool wasm_exception(struct trap_frame *f, struct arch_thread_context *context, int signal, uint64_t address,
                    const char *message, size_t len) {
  active_context = context;
  fault_pc = f->elr_el1;
  fault_text = message;
  fault_len = len;
  host_exception(fault_pc, message, len);
  const struct vma *vma = signal == 11 ? vma_lookup(cell_domain_vmas(cpus[cpu_id].thread->domain), address) : NULL;
  if (vma && log_console) {
    struct vfs_stats stats = vfs_get_stats();
    kprintf("[kernel] fault in mapped guest memory, free pages %u, cached pages %u/%u\n", (unsigned)pmm_free_pages(),
            (unsigned)stats.page_cache_pages, (unsigned)stats.page_cache_capacity);
  }
  int code = vma ? 2 : 1;
  cell_signal_current_fault(signal, f, address, code);
  fault_len = 0;
  fault_text = NULL;
  active_context = NULL;
  return cpus[cpu_id].idle;
}

bool wasm_timer(struct trap_frame *f, struct arch_thread_context *context, unsigned ticks, bool network_ready) {
  active_context = context;
  if (!cpus[cpu_id].idle && (!cpus[cpu_id].thread || cpus[cpu_id].thread->state != THREAD_RUNNABLE)) {
    cell_schedule(f);
  }
  if (network_ready && !ticks) {
    net_poll();
    cell_wake_poll_waiters_internal();
  }
  while (ticks--) {
    if (!cpu_id) {
      wasm_poll_input();
      struct trap_frame *active = cpus[0].idle ? NULL : f;
      uint32_t size = host_winsize();
      if (size != dimensions) {
        dimensions = size;
        cell_tty_signal_foreground(28, active);
      }
      cell_wake_stdin(active);
    }
    cell_timer_tick(f, !cpus[cpu_id].idle);
  }
  if (cpus[cpu_id].idle) { cell_resume_scheduler(f); }
  wasm_flush();
  active_context = NULL;
  return cpus[cpu_id].idle;
}

size_t proc_cpuinfo_text(char *out, size_t cap) {
  size_t len = 0;
  for (uint32_t cpu = 0; cpu < cpu_count; ++cpu) {
    proc_append_str(out, cap, &len, "processor\t: ");
    proc_append_u64(out, cap, &len, cpu);
    proc_append_str(out, cap, &len,
                    "\nmodel name\t: Spore ARM64 virtual CPU\nCPU architecture: 8\n"
                    "Features\t: fp asimd aes pmull sha1 sha2 crc32 atomics\n\n");
  }
  return len;
}
