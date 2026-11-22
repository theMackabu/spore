#include "arch/aarch64/timer.h"

#include "arch/aarch64/smp.h"
#include "cell.h"
#include "kprintf.h"
#include "pl011.h"

enum {
  VIRTUAL_TIMER_INTID = 27,
  PL011_INTID = 33,
  SPURIOUS_INTID = 1023,
  GICD_BASE = 0x08000000,
  GICR_BASE = 0x080a0000,
  GICR_SGI_BASE = 0x080b0000,
  GICD_CTLR = 0x0000,
  GICD_IGROUPR = 0x0080,
  GICD_ISENABLER = 0x0100,
  GICD_IPRIORITYR = 0x0400,
  GICR_WAKER = 0x0014,
  GICR_IGROUPR0 = 0x0080,
  GICR_ISENABLER0 = 0x0100,
  GICR_IPRIORITYR = 0x0400,
};

static uint32_t timer_interval;
static uint64_t tick_count;
static uint64_t gic_hhdm_offset;

static volatile uint32_t *mmio32(uint64_t phys) {
  return (volatile uint32_t *)(uintptr_t)(gic_hhdm_offset + phys);
}

static uint32_t mmio_read32(uint64_t phys) {
  return *mmio32(phys);
}

static void mmio_write32(uint64_t phys, uint32_t value) {
  *mmio32(phys) = value;
  __asm__ volatile("dsb sy" : : : "memory");
}

static void gic_wait_rwp(void) {
  while ((mmio_read32(GICD_BASE + GICD_CTLR) & (1u << 31)) != 0) {}
}

static void gic_dist_init(void) {
  mmio_write32(GICD_BASE + GICD_CTLR, 0);
  gic_wait_rwp();
  mmio_write32(GICD_BASE + GICD_CTLR, (1u << 4) | (1u << 1));
  gic_wait_rwp();
}

static uint64_t gic_redist_base(uint32_t cpu) {
  return GICR_BASE + (uint64_t)cpu * 0x20000ull;
}

static uint64_t gic_sgi_base(uint32_t cpu) {
  return gic_redist_base(cpu) + 0x10000ull;
}

static void gic_redist_init(uint32_t cpu) {
  uint64_t redist = gic_redist_base(cpu);
  uint64_t sgi = gic_sgi_base(cpu);
  uint32_t waker = mmio_read32(redist + GICR_WAKER);
  waker &= ~(1u << 1);
  mmio_write32(redist + GICR_WAKER, waker);
  while ((mmio_read32(redist + GICR_WAKER) & (1u << 2)) != 0) {}

  mmio_write32(sgi + GICR_IGROUPR0, 1u << VIRTUAL_TIMER_INTID);

  uint64_t priority_reg = sgi + GICR_IPRIORITYR + (VIRTUAL_TIMER_INTID & ~3u);
  uint32_t priority = mmio_read32(priority_reg);
  priority &= ~(0xffu << ((VIRTUAL_TIMER_INTID & 3u) * 8u));
  priority |= 0x80u << ((VIRTUAL_TIMER_INTID & 3u) * 8u);
  mmio_write32(priority_reg, priority);

  mmio_write32(sgi + GICR_ISENABLER0, 1u << VIRTUAL_TIMER_INTID);
}

static void gic_enable_spi(uint32_t intid) {
  uint32_t bit = 1u << (intid % 32u);
  mmio_write32(GICD_BASE + GICD_IGROUPR + (intid / 32u) * 4u, bit);

  uint64_t priority_reg = GICD_BASE + GICD_IPRIORITYR + (intid & ~3u);
  uint32_t priority = mmio_read32(priority_reg);
  priority &= ~(0xffu << ((intid & 3u) * 8u));
  priority |= 0x80u << ((intid & 3u) * 8u);
  mmio_write32(priority_reg, priority);

  mmio_write32(GICD_BASE + GICD_ISENABLER + (intid / 32u) * 4u, bit);
}

static void gic_cpu_init(void) {
  uint64_t sre;
  __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre)); // ICC_SRE_EL1
  sre |= 1;
  __asm__ volatile("msr S3_0_C12_C12_5, %0\n" // ICC_SRE_EL1
                   "isb\n"
                   "msr S3_0_C4_C6_0, %1\n"   // ICC_PMR_EL1
                   "msr S3_0_C12_C12_7, %2\n" // ICC_IGRPEN1_EL1
                   "isb\n"
                   :
                   : "r"(sre), "r"(0xffull), "r"(1ull)
                   : "memory");
}

void timer_init(uint64_t hhdm_offset) {
  gic_hhdm_offset = hhdm_offset;
  uint64_t freq;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  uint64_t cntkctl;
  __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(cntkctl));
  cntkctl |= 3ull; // EL0PCTEN | EL0VCTEN: userland may read generic counters.
  __asm__ volatile("msr cntkctl_el1, %0" : : "r"(cntkctl) : "memory");
  uint64_t cpacr;
  __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
  cpacr |= 3ull << 20; // FPEN: EL0/EL1 may use FP/SIMD; scheduler saves it per thread.
  __asm__ volatile("msr cpacr_el1, %0\nisb" : : "r"(cpacr) : "memory");
  uint64_t sctlr;
  __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  sctlr |= 1ull << 15; // UCT: userland may read CTR_EL0 like Linux permits.
  sctlr |= 1ull << 26; // UCI: userland JITs may issue cache maintenance.
  __asm__ volatile("msr sctlr_el1, %0\nisb" : : "r"(sctlr) : "memory");
  timer_interval = (uint32_t)(freq / 100);
  if (timer_interval == 0) { timer_interval = 1; }
  gic_dist_init();
  gic_redist_init(0);
  gic_enable_spi(PL011_INTID);
  gic_cpu_init();
  pl011_enable_rx_irq();
  __asm__ volatile("msr cntv_tval_el0, %0\n"
                   "msr cntv_ctl_el0, %1\n"
                   "isb\n"
                   :
                   : "r"((uint64_t)timer_interval), "r"(1ull)
                   : "memory");
  kprintf("[spore] scheduler 100Hz, gicv3 up\n");
}

void timer_cpu_init(uint32_t cpu) {
  uint64_t cntkctl;
  __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(cntkctl));
  cntkctl |= 3ull;
  __asm__ volatile("msr cntkctl_el1, %0" : : "r"(cntkctl) : "memory");
  uint64_t cpacr;
  __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
  cpacr |= 3ull << 20;
  __asm__ volatile("msr cpacr_el1, %0\nisb" : : "r"(cpacr) : "memory");
  uint64_t sctlr;
  __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
  sctlr |= 1ull << 15;
  sctlr |= 1ull << 26;
  __asm__ volatile("msr sctlr_el1, %0\nisb" : : "r"(sctlr) : "memory");
  gic_redist_init(cpu);
  gic_cpu_init();
  __asm__ volatile("msr cntv_tval_el0, %0\n"
                   "msr cntv_ctl_el0, %1\n"
                   "isb\n"
                   :
                   : "r"((uint64_t)timer_interval), "r"(1ull)
                   : "memory");
}

static uint64_t irq_ack(void) {
  uint64_t iar;
  __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(iar)); // ICC_IAR1_EL1
  return iar;
}

static void irq_eoi(uint64_t iar) {
  __asm__ volatile("msr S3_0_C12_C12_1, %0\n" // ICC_EOIR1_EL1
                   "isb\n"
                   :
                   : "r"(iar)
                   : "memory");
}

void handle_irq(struct trap_frame *frame, uint64_t from_lower_el) {
  uint64_t iar = irq_ack();
  uint32_t intid = (uint32_t)(iar & 0xffffffu);
  if (intid == VIRTUAL_TIMER_INTID) {
    ++tick_count;
    __asm__ volatile("msr cntv_tval_el0, %0\n"
                     "msr cntv_ctl_el0, %1\n"
                     "isb\n"
                     :
                     : "r"((uint64_t)timer_interval), "r"(1ull)
                     : "memory");
    if (pl011_poll_rx()) { cell_wake_stdin(from_lower_el != 0 ? frame : NULL); }
    irq_eoi(iar);
    cell_timer_tick(frame, from_lower_el != 0);
    return;
  }
  if (intid == PL011_INTID) {
    if (pl011_handle_irq()) { cell_wake_stdin(from_lower_el != 0 ? frame : NULL); }
    irq_eoi(iar);
    return;
  }
  if (intid != SPURIOUS_INTID) { irq_eoi(iar); }
}
