#include "arch/platform.h"
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
#include "proc/signal.h"
#include "proc/thread.h"
#include "tty/tty.h"

#define HOST(name) __attribute__((import_module("spore"), import_name(name)))
HOST("clock") uint64_t host_clock(void);
HOST("random") void host_random(void *, size_t);
HOST("net_recv") int host_net_recv(void *, size_t);
HOST("net_send") int host_net_send(const void *, size_t);
HOST("disk_write") int host_disk_write(uint64_t, const void *, size_t);
HOST("exception") void host_exception(uint64_t, const char *, size_t);
HOST("serial_read") int host_serial_read(void);
HOST("disk_read") int host_disk_read(uint64_t, void *, size_t);
HOST("write") void host_write(unsigned, const void *, size_t);
HOST("finish") [[noreturn]] void host_finish(int, unsigned, uint64_t, const char *, size_t);

static alignas(4096) uint8_t ram[64 * 1024 * 1024];
static struct thread *current;
static uint8_t output[4096];
static unsigned channel = 1, buffered;
static struct arch_thread_context *active_context;
static bool idle;
static uint64_t fault_pc;
static const char *fault_text;
static size_t fault_len;

static void wasm_flush(void) {
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
void serial_get_winsize(uint16_t *rows, uint16_t *cols) {
  *rows = 24;
  *cols = 80;
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

uint32_t smp_present_cpu_count(void) {
  return 1;
}
uint32_t smp_possible_cpu_count(void) {
  return 1;
}
uint32_t smp_online_cpu_count(void) {
  return 1;
}
uint32_t smp_current_cpu(void) {
  return 0;
}
bool smp_cpu_present(uint32_t cpu) {
  return cpu == 0;
}
bool smp_cpu_online(uint32_t cpu) {
  return cpu == 0;
}
uint64_t smp_cpu_hardware_id(uint32_t cpu) {
  return cpu;
}
void *smp_current_thread_slot(uint32_t cpu) {
  return cpu == 0 ? current : NULL;
}
void smp_set_current_thread_slot(uint32_t cpu, void *t) {
  if (!cpu) { current = t; }
}
void smp_clear_current_thread_if(uint32_t cpu, void *t) {
  if (!cpu && current == t) { current = NULL; }
}
bool smp_scheduler_waiting(uint32_t cpu) {
  (void)cpu;
  return idle;
}
void smp_set_scheduler_waiting(uint32_t cpu, bool waiting) {
  (void)cpu;
  idle = waiting;
}
void smp_kernel_lock(void) {}
void smp_kernel_unlock(void) {}
void smp_notify(void) {}

void aarch64_publish_table(uint64_t *slot, uint64_t entry) {
  *slot = entry;
}
void aarch64_tlb_flush_va(uint64_t va) {
  (void)va;
}
void aarch64_tlb_flush_pages(uint64_t va, uint64_t mask) {
  (void)va;
  (void)mask;
}
void aarch64_tlb_flush_all(void) {}
void vmm_install_user(const struct user_address_space *as) {
  (void)as;
}

void arch_save_context(struct arch_thread_context *out) {
  *out = *active_context;
}
void arch_restore_context(const struct arch_thread_context *in) {
  *active_context = *in;
}

bool virtio_console_ready(void) {
  return false;
}
void virtio_console_write(const uint8_t *p, size_t n) {
  (void)p;
  (void)n;
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
  (void)off;
  (void)p;
  (void)n;
  return false;
}
static bool disk_read(void *ctx, uint64_t off, void *p, uint32_t n) {
  (void)ctx;
  return virtio_blk_read(off, p, n);
}

static bool disk_write(void *ctx, uint64_t off, const void *p, uint32_t n) {
  (void)ctx;
  return virtio_blk_write(off, p, n);
}

void app_finish(enum app_result kind, unsigned value) {
  if (!vfs_shutdown()) { kind = APP_BOOT_ERROR; value = BOOT_FLUSH_FAILED; }
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
  arch_poweroff();
}

struct trap_frame *wasm_boot(const uint8_t *image, size_t size, const char *args, size_t bytes, unsigned devices,
                             uint64_t epoch) {
  const uint64_t hhdm = (uintptr_t)ram;
  struct spore_boot_module payload = {.phys_addr = (uint64_t)(uintptr_t)image - hhdm, .size = size, .path = "/spore.app"};
  struct spore_boot_info boot = {.hhdm_offset = hhdm, .modules = size ? &payload : NULL, .module_count = size ? 1 : 0};
  struct boot_profile profile = {0};
  boot_select_profile(&boot, &profile);
  const struct spore_memmap_entry usable = {
    .base = 0x100000, .length = sizeof(ram) - 0x100000, .type = SPORE_MEMMAP_USABLE};
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
  if (first.as.freebsd_abi) { boot_fail(BOOT_INVALID_PAYLOAD, "expected a Linux ELF"); }
  boot_activate_process(&first);
  syscall_set_address_space(cell_current_as());
  if (!(devices & 4)) {
    struct vfs_node null;
    if (!vfs_lookup("/dev/null", &null) || cell_fd_close(0) ||
        cell_fd_open_node(&null, 0, "/dev/null", "/dev/null", NULL) != 0) {
      boot_fail(BOOT_INIT_FAILED, "cannot initialize stdin");
    }
    cell_tty_set_oflag(0);
  }
  return &current->tf;
}

void *wasm_translate(uint64_t page, enum vmm_access access) {
  struct user_address_space *as = cell_current_as();
  if (page >= USER_VA_LIMIT || !cell_domain_ensure_user_range(current->domain, page, 1, access)) { return NULL; }
  if (!vmm_user_range_accessible(as, page, 1, access)) {
    if (access != VMM_ACCESS_WRITE || !cell_handle_write_fault(page)) { return NULL; }
  }
  uint64_t pa = vmm_user_to_phys(as, page);
  return pa ? pmm_phys_to_virt(pa) : NULL;
}

bool wasm_syscall(struct trap_frame *f, struct arch_thread_context *context) {
  active_context = context;
  uint64_t nr = f->x[8];
  channel = (nr == SYS_WRITE || nr == SYS_WRITEV) && f->x[0] == 2 ? 2 : 1;
  linux_aarch64_syscall(f);
  wasm_flush();
  active_context = NULL;
  return idle;
}

bool wasm_exception(struct trap_frame *f, struct arch_thread_context *context, int signal, uint64_t address,
                    const char *message, size_t len) {
  active_context = context;
  fault_pc = f->elr_el1;
  fault_text = message;
  fault_len = len;
  host_exception(fault_pc, message, len);
  int code = signal == 11 && vma_lookup(cell_domain_vmas(current->domain), address) ? 2 : 1;
  cell_signal_current_fault(signal, f, address, code);
  fault_len = 0;
  fault_text = NULL;
  active_context = NULL;
  return idle;
}

bool wasm_timer(struct trap_frame *f, struct arch_thread_context *context, unsigned ticks) {
  active_context = context;
  while (ticks--) {
    cell_wake_stdin(idle ? NULL : f);
    cell_timer_tick(f, !idle);
    if (idle) { cell_resume_scheduler(f); }
  }
  wasm_flush();
  active_context = NULL;
  return idle;
}

size_t proc_cpuinfo_text(char *out, size_t cap) {
  const char description[] = "processor\t: 0\nmodel name\t: Spore ARM64 interpreter\n";
  if (!cap) { return 0; }
  size_t count = cap < sizeof(description) ? cap - 1 : sizeof(description) - 1;
  kmemcpy(out, description, count);
  out[count] = 0;
  return count;
}
