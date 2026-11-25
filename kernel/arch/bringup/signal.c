#include "abi/linux/shared/signal.h"
#include "arch/regs.h"
#include "arch/signal_frame.h"
#include "exec/stack.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/signal.h"
#include "proc/thread.h"

constexpr uint64_t trampoline = USER_STACK_TOP;
bool port_install_trampoline(struct user_address_space *as, struct vma_list *vmas) {
  if (!vma_insert(vmas, trampoline, trampoline + PAGE_SIZE, VMM_USER_READ | VMM_USER_EXEC, 0, VMA_ANON) ||
      !vmm_alloc_page(as, trampoline, VMM_USER_READ | VMM_USER_EXEC)) {
    return false;
  }
#ifdef __x86_64__
  static const unsigned char code[] = {0xb8, 15, 0, 0, 0, 0x0f, 0x05, 0x0f, 0x0b};
#else
  static const uint32_t code[] = {0x08b00893, 0x00000073, 0};
#endif
  uint8_t *dest = pmm_phys_to_virt(vmm_user_to_phys(as, trampoline));
  __builtin_memcpy(dest, code, sizeof(code));
#ifdef __riscv
  __asm__ volatile("fence.i" ::: "memory");
#endif
  return true;
}

struct context {
  uint64_t flags, link;
  struct stack_t64 stack;
#ifdef __x86_64__
  uint64_t reg[23], fp, reserved[8], mask;
#else
  uint64_t mask, padding[16], reg[32];
  alignas(16) uint8_t fp[528];
#endif
};
struct native_sigframe {
#ifdef __x86_64__
  uint64_t restorer;
  struct context ctx;
  struct siginfo64 info;
  uint8_t fpstate[512 + 63];
#else
  struct siginfo64 info;
  struct context ctx;
#endif
};
#ifdef __x86_64__
static void save_regs(uint64_t *r, const struct trap_frame *f) {
  uint64_t g[] = {f->r8,  f->r9,  f->r10, f->r11, f->r12, f->r13, f->r14, f->r15,    f->rdi, f->rsi,
                  f->rbp, f->rbx, f->rdx, f->rax, f->rcx, f->rsp, f->rip, f->rflags, 0x23};
  __builtin_memcpy(r, g, sizeof(g));
}
static bool load_regs(struct trap_frame *f, const uint64_t *r) {
  if (r[15] >= USER_VA_LIMIT || r[16] >= USER_VA_LIMIT) return false;
  *f = (struct trap_frame){.r8 = r[0],
                           .r9 = r[1],
                           .r10 = r[2],
                           .r11 = r[3],
                           .r12 = r[4],
                           .r13 = r[5],
                           .r14 = r[6],
                           .r15 = r[7],
                           .rdi = r[8],
                           .rsi = r[9],
                           .rbp = r[10],
                           .rbx = r[11],
                           .rdx = r[12],
                           .rax = r[13],
                           .rcx = r[14],
                           .rsp = r[15],
                           .rip = r[16],
                           .rflags = (f->rflags & 0x200) | (r[17] & 0xcd5) | 2,
                           .cs = 0x23,
                           .ss = 0x1b};
  return true;
}
#else
static void save_regs(uint64_t *r, const struct trap_frame *f) {
  r[0] = f->pc;
  r[1] = f->ra;
  r[2] = f->sp;
  r[3] = f->gp;
  r[4] = f->tp;
  __builtin_memcpy(r + 5, f->t, 24);
  __builtin_memcpy(r + 8, f->s, 16);
  __builtin_memcpy(r + 10, f->a, 64);
  __builtin_memcpy(r + 18, f->s + 2, 80);
  __builtin_memcpy(r + 28, f->t + 3, 32);
}
static bool load_regs(struct trap_frame *f, const uint64_t *r) {
  if (r[0] >= USER_VA_LIMIT || r[2] >= USER_VA_LIMIT) return false;
  f->pc = r[0];
  f->ra = r[1];
  f->sp = r[2];
  f->gp = r[3];
  f->tp = r[4];
  __builtin_memcpy(f->t, r + 5, 24);
  __builtin_memcpy(f->s, r + 8, 16);
  __builtin_memcpy(f->a, r + 10, 64);
  __builtin_memcpy(f->s + 2, r + 18, 80);
  __builtin_memcpy(f->t + 3, r + 28, 32);
  return true;
}
#endif

bool arch_push_signal_frame(struct thread *t, const struct signal_action *action, const struct siginfo_fields64 *info) {
  int sig = info->si_signo;
  struct native_sigframe sf = {0};
  struct trap_frame *f = &t->tf;
  uint64_t sp = trap_user_stack(f);
  bool on_alt = !(t->sigaltstack_flags & 2) && sp >= t->sigaltstack_sp && sp - t->sigaltstack_sp < t->sigaltstack_size;
  sf.ctx.stack = (struct stack_t64){
    .ss_sp = t->sigaltstack_sp, .ss_flags = on_alt ? 1 : t->sigaltstack_flags, .ss_size = t->sigaltstack_size};
  sf.ctx.mask = t->signal_mask;
  sf.info.fields = *info;
  save_regs(sf.ctx.reg, f);
#ifdef __x86_64__
  sp -= 128;
#endif
  if ((action->flags & 0x08000000) && !on_alt && !(t->sigaltstack_flags & 2))
    sp = t->sigaltstack_sp + t->sigaltstack_size;
  uint64_t base = (sp - sizeof(sf)) & ~15ull;
#ifdef __x86_64__
  base -= 8;
  sf.restorer = action->restorer ? action->restorer : trampoline;
  sf.ctx.fp = (base + offsetof(struct native_sigframe, fpstate) + 63) & ~63ull;
  uint8_t *fp = (uint8_t *)&sf + (sf.ctx.fp - base);
#else
  uint8_t *fp = sf.ctx.fp;
#endif
  if (t == cell_current_thread_internal()) { arch_save_context(&t->context); }
  __builtin_memcpy(fp, t->context.fp, sizeof(t->context.fp));
  if (!cell_domain_ensure_user_range(t->domain, base, sizeof(sf), VMM_ACCESS_WRITE) ||
      !vmm_copy_to_user(cell_domain_as(t->domain), base, &sf, sizeof(sf)))
    return false;
#ifdef __x86_64__
  f->rip = action->handler;
  f->rsp = base;
  f->rdi = sig;
  f->rsi = base + offsetof(struct native_sigframe, info);
  f->rdx = base + offsetof(struct native_sigframe, ctx);
#else
  f->pc = action->handler;
  f->ra = trampoline;
  f->sp = base;
  f->a[0] = sig;
  f->a[1] = base;
  f->a[2] = base + offsetof(struct native_sigframe, ctx);
#endif
  return true;
}
int cell_rt_sigreturn(struct trap_frame *f) {
  struct native_sigframe sf;
  uint64_t base = trap_user_stack(f);
#ifdef __x86_64__
  base -= 8;
#endif
  struct trap_frame next = *f;
  if (!vmm_copy_from_user(cell_current_as(), &sf, base, sizeof(sf)) || !load_regs(&next, sf.ctx.reg)) { return -14; }
  struct thread *t = cell_current_thread_internal();
  struct arch_thread_context context;
  arch_save_context(&context);
#ifdef __x86_64__
  if (sf.ctx.fp) {
    uint32_t mask, mxcsr;
    __builtin_memcpy(&mask, context.fp + 28, sizeof(mask));
    if (!vmm_copy_from_user(cell_current_as(), context.fp, sf.ctx.fp, sizeof(context.fp))) { return -14; }
    __builtin_memcpy(&mxcsr, context.fp + 24, sizeof(mxcsr));
    if (mxcsr & ~(mask ? mask : 0xffbfu)) { return -22; }
  }
#else
  __builtin_memcpy(context.fp, sf.ctx.fp, sizeof(context.fp));
  context.thread_pointer = next.tp;
#endif
  arch_restore_context(&context);
  *f = next;
  t->signal_mask = cell_sanitize_signal_mask(sf.ctx.mask);
  t->tf = *f;
  return 0;
}
