#include "abi/linux/aarch64/signal_context.h"
#include "arch/signal_frame.h"

#include "lib/kprintf.h"
#include "lib/mem.h"
#include "proc/domain.h"
#include "proc/memory.h"
#include "proc/signal.h"
#include "proc/thread.h"

enum {
  EFAULT = 14,
  SIGSEGV = 11,
  SA_SIGINFO = 4,
  SA_ONSTACK = 0x08000000,
  SS_ONSTACK = 1,
  SS_DISABLE = 2
};

struct signal_frame64 {
  struct siginfo64 siginfo;
  struct ucontext64 ucontext;
  uint64_t magic;
  uint64_t signal;
  uint64_t saved_signal_mask;
  struct trap_frame saved;
};

static bool thread_on_sigaltstack(const struct thread *thread) {
  if (thread == NULL || (thread->sigaltstack_flags & SS_DISABLE) != 0 || thread->sigaltstack_size == 0) {
    return false;
  }
  uint64_t sp = thread->tf.sp_el0;
  uint64_t end = thread->sigaltstack_sp + thread->sigaltstack_size;
  return end >= thread->sigaltstack_sp && sp >= thread->sigaltstack_sp && sp < end;
}

static void fill_signal_context(struct signal_frame64 *frame, const struct thread *thread, int signal,
                                uint64_t fault_addr, int sig_code) {
  frame->siginfo.fields.si_signo = signal;
  frame->siginfo.fields.si_code = sig_code;
  frame->siginfo.fields.si_addr = fault_addr;

  frame->ucontext.uc_stack.ss_sp = thread->sigaltstack_sp;
  frame->ucontext.uc_stack.ss_flags = thread_on_sigaltstack(thread) ? SS_ONSTACK : thread->sigaltstack_flags;
  frame->ucontext.uc_stack.ss_size = thread->sigaltstack_size;
  frame->ucontext.uc_sigmask = thread->signal_mask;
  frame->ucontext.uc_mcontext.fault_address = fault_addr;
  for (size_t i = 0; i < 31; ++i) {
    frame->ucontext.uc_mcontext.regs[i] = thread->tf.x[i];
  }
  frame->ucontext.uc_mcontext.sp = thread->tf.sp_el0;
  frame->ucontext.uc_mcontext.pc = thread->tf.elr_el1;
  frame->ucontext.uc_mcontext.pstate = thread->tf.spsr_el1;
}

bool arch_push_signal_frame(struct thread *thread, const struct signal_action *action, int signal,
                               uint64_t fault_addr, int sig_code) {
  if (!action->restorer) { return false; }
  struct domain *domain = thread->domain;
  struct signal_frame64 frame;
  kmemset(&frame, 0, sizeof(frame));
  frame.magic = 0x5350475349474652ull;
  frame.signal = (uint64_t)signal;
  frame.saved_signal_mask = thread->signal_mask;
  frame.saved = thread->tf;
  fill_signal_context(&frame, thread, signal, fault_addr, sig_code);

  uint64_t stack_top = thread->tf.sp_el0;
  if ((action->flags & SA_ONSTACK) != 0 && !thread_on_sigaltstack(thread) &&
      (thread->sigaltstack_flags & SS_DISABLE) == 0 && thread->sigaltstack_size >= sizeof(frame)) {
    stack_top = thread->sigaltstack_sp + thread->sigaltstack_size;
  }
  uint64_t frame_addr = (stack_top - sizeof(frame)) & ~15ull;
  if (!cell_domain_ensure_user_range(domain, frame_addr, sizeof(frame), VMM_ACCESS_WRITE) ||
      !vmm_copy_to_user(cell_domain_as(domain), frame_addr, &frame, sizeof(frame))) {
    return false;
  }

  uint64_t siginfo_addr = frame_addr + offsetof(struct signal_frame64, siginfo);
  uint64_t ucontext_addr = frame_addr + offsetof(struct signal_frame64, ucontext);
  thread->tf.x[0] = (uint64_t)signal;
  thread->tf.x[1] = (action->flags & SA_SIGINFO) != 0 ? siginfo_addr : 0;
  thread->tf.x[2] = (action->flags & SA_SIGINFO) != 0 ? ucontext_addr : 0;
  thread->tf.x[30] = action->restorer;
  thread->tf.sp_el0 = frame_addr;
  thread->tf.elr_el1 = action->handler;
  return true;
}

int cell_rt_sigreturn(struct trap_frame *frame) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -EFAULT; }
  struct signal_frame64 sigframe;
  uint64_t frame_addr = frame->sp_el0;
  if (!cell_domain_ensure_user_range(domain, frame_addr, sizeof(sigframe), VMM_ACCESS_READ) ||
      !vmm_copy_from_user(cell_domain_as(domain), &sigframe, frame_addr, sizeof(sigframe)) ||
      sigframe.magic != 0x5350475349474652ull) {
    cell_signal_current(SIGSEGV, frame);
    return -EFAULT;
  }
  *frame = sigframe.saved;
  for (size_t i = 0; i < 31; ++i) {
    frame->x[i] = sigframe.ucontext.uc_mcontext.regs[i];
  }
  frame->sp_el0 = sigframe.ucontext.uc_mcontext.sp;
  frame->elr_el1 = sigframe.ucontext.uc_mcontext.pc;
  frame->spsr_el1 = 0x340 | (sigframe.ucontext.uc_mcontext.pstate & 0xf0000000ull);
  struct thread *thread = cell_current_thread_internal();
  if (thread != NULL) {
    thread->tf = *frame;
    thread->signal_mask = cell_sanitize_signal_mask(sigframe.saved_signal_mask);
    (void)cell_deliver_pending_signals_current(frame);
  }
  return 0;
}

void cell_dump_current_fault(const struct trap_frame *frame, uint64_t far) {
  struct domain *domain = cell_current_domain_internal();
  struct thread *thread = cell_current_thread_internal();
  if (domain == NULL || thread == NULL) {
    kprintf("[kernel] fault: no current domain esr=%x elr=%p far=%p\n", frame == NULL ? 0 : (unsigned)frame->esr_el1,
            frame == NULL ? NULL : (void *)(uintptr_t)frame->elr_el1, (void *)(uintptr_t)far);
    return;
  }
  kprintf(
    "[kernel] fault: pid=%d tid=%d cmd=%s cwd=%s esr=%x elr=%p far=%p sp=%p x0=%p x1=%p x2=%p x3=%p x8=%p x16=%p "
    "x17=%p x19=%p x20=%p x21=%p x22=%p x29=%p x30=%p\n",
    domain->id, thread->tid, domain->cmdline[0] != '\0' ? domain->cmdline : domain->name,
    domain->cwd[0] != '\0' ? domain->cwd : "/", frame == NULL ? 0 : (unsigned)frame->esr_el1,
    frame == NULL ? NULL : (void *)(uintptr_t)frame->elr_el1, (void *)(uintptr_t)far,
    frame == NULL ? NULL : (void *)(uintptr_t)frame->sp_el0, frame == NULL ? NULL : (void *)(uintptr_t)frame->x[0],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[1], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[2],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[3], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[8],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[16], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[17],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[19], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[20],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[21], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[22],
    frame == NULL ? NULL : (void *)(uintptr_t)frame->x[29], frame == NULL ? NULL : (void *)(uintptr_t)frame->x[30]);
  if (frame != NULL) {
    uint64_t words[4] = {0};
    if (vmm_copy_from_user(cell_domain_as(domain), words, frame->sp_el0, sizeof(words))) {
      kprintf("[kernel] fault stack: %p %p %p %p\n", (void *)(uintptr_t)words[0], (void *)(uintptr_t)words[1],
              (void *)(uintptr_t)words[2], (void *)(uintptr_t)words[3]);
    }
    if (frame->x[19] != 0 && vmm_copy_from_user(cell_domain_as(domain), words, frame->x[19], sizeof(words))) {
      kprintf("[kernel] fault x19 mem: %p %p %p %p\n", (void *)(uintptr_t)words[0], (void *)(uintptr_t)words[1],
              (void *)(uintptr_t)words[2], (void *)(uintptr_t)words[3]);
    }
    const struct vma_list *vmas = cell_domain_vmas_const(domain);
    for (size_t i = 0; i < vma_count(vmas); ++i) {
      const struct vma *vma = vma_at(vmas, i);
      bool interesting = (frame->sp_el0 >= vma->start && frame->sp_el0 < vma->end) ||
                         (frame->elr_el1 >= vma->start && frame->elr_el1 < vma->end) ||
                         (frame->x[19] >= vma->start && frame->x[19] < vma->end) ||
                         (far >= vma->start && far < vma->end);
      if (interesting) {
        kprintf("[kernel] fault vma: %p-%p prot=%x type=%d\n", (void *)(uintptr_t)vma->start,
                (void *)(uintptr_t)vma->end, (unsigned)vma->prot, (int)vma->type);
      }
    }
  }
}
