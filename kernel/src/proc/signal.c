#include "proc/signal.h"

#include "kprintf.h"
#include "mem.h"
#include "proc/domain.h"
#include "proc/fd.h"
#include "proc/memory.h"
#include "proc/process.h"
#include "proc/thread.h"

#include <stddef.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
  EINTR = 4,
  ESRCH = 3,
  SIGINT = 2,
  SIGABRT = 6,
  SIGKILL = 9,
  SIGSEGV = 11,
  SIGPIPE = 13,
  SIGTERM = 15,
  SA_SIGINFO = 4,
  NSIG = 65,
};

static const uint64_t SA_RESETHAND = 0x80000000ull;

struct k_sigaction64 {
  uint64_t handler;
  uint64_t flags;
  uint64_t restorer;
  uint32_t mask[2];
};

struct signal_frame64 {
  uint64_t magic;
  uint64_t signal;
  struct trap_frame saved;
  uint8_t siginfo[128];
  uint8_t ucontext[1024];
};

static struct domain *current_domain(void) {
  return cell_current_domain_internal();
}

static bool signal_is_supported(int signal) {
  return signal == SIGTERM || signal == SIGINT || signal == SIGABRT || signal == SIGKILL || signal == SIGSEGV ||
         signal == SIGPIPE;
}

static void terminate_domain_by_signal(struct domain *domain, int signal) {
  if (domain == NULL || domain->zombie) { return; }
  domain->exit_status = 128 + signal;
  domain->term_signal = signal;
  domain->zombie = true;
  cell_close_all_fds(domain);
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == domain) {
      thread->state = THREAD_ZOMBIE;
      if (thread->running_cpu < 0) { thread->running_cpu = -1; }
    }
  }
  cell_wake_parent_of(domain);
}

bool cell_deliver_signal_to_thread(struct thread *thread, int signal) {
  if (thread == NULL || thread->domain == NULL || signal <= 0 || signal >= (int)NSIG) { return false; }
  struct domain *domain = thread->domain;
  struct signal_action *action = &domain->signal_actions[signal];
  if (action->handler == 0 || signal == SIGKILL) {
    terminate_domain_by_signal(domain, signal);
    return true;
  }
  if (action->handler == 1) { return true; }
  if (action->restorer == 0) {
    terminate_domain_by_signal(domain, signal);
    return true;
  }

  if (thread->state == THREAD_BLOCKED) {
    thread->tf.x[0] = (uint64_t)(-(int64_t)EINTR);
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    thread->wait_target = -1;
    thread->stdin_buf = 0;
    thread->stdin_len = 0;
    thread->pipe_buf = 0;
    thread->pipe_len = 0;
    thread->socket_addr = 0;
    thread->socket_addrlen = 0;
    thread->socket_accept_flags = 0;
    thread->socket_write = false;
    thread->socket_msg = false;
    thread->socket_msg_addr = 0;
    thread->socket_iov = 0;
    thread->socket_iovlen = 0;
    thread->socket_flags = 0;
    thread->socket_has_deadline = false;
    thread->socket_deadline_tick = 0;
  }

  struct signal_frame64 frame;
  kmemset(&frame, 0, sizeof(frame));
  frame.magic = 0x5350475349474652ull; // "SPGSIGFR"
  frame.signal = (uint64_t)signal;
  frame.saved = thread->tf;

  uint64_t frame_addr = (thread->tf.sp_el0 - sizeof(frame)) & ~15ull;
  if (!cell_domain_ensure_user_range(domain, frame_addr, sizeof(frame), VMM_ACCESS_WRITE) ||
      !vmm_copy_to_user(cell_domain_as(domain), frame_addr, &frame, sizeof(frame))) {
    terminate_domain_by_signal(domain, SIGSEGV);
    return true;
  }

  uint64_t siginfo_addr = frame_addr + offsetof(struct signal_frame64, siginfo);
  uint64_t ucontext_addr = frame_addr + offsetof(struct signal_frame64, ucontext);
  thread->tf.x[0] = (uint64_t)signal;
  thread->tf.x[1] = (action->flags & SA_SIGINFO) != 0 ? siginfo_addr : 0;
  thread->tf.x[2] = (action->flags & SA_SIGINFO) != 0 ? ucontext_addr : 0;
  thread->tf.x[30] = action->restorer;
  thread->tf.sp_el0 = frame_addr;
  thread->tf.elr_el1 = action->handler;
  if ((action->flags & SA_RESETHAND) != 0) { action->handler = 0; }
  return true;
}

bool cell_signal_current(int signal, struct trap_frame *frame) {
  struct thread *thread = cell_current_thread_internal();
  if (thread == NULL || frame == NULL) { return false; }
  bool ignored = thread->domain != NULL && signal > 0 && signal < (int)NSIG &&
                 thread->domain->signal_actions[signal].handler == 1 && signal != SIGKILL;
  thread->tf = *frame;
  bool delivered = cell_deliver_signal_to_thread(thread, signal);
  *frame = thread->tf;
  if (thread->state == THREAD_ZOMBIE) {
    cell_schedule(frame);
    return true;
  }
  return delivered && !ignored;
}

int cell_rt_sigaction(int signal, uint64_t act_addr, uint64_t old_addr, uint64_t sigset_size) {
  struct domain *domain = current_domain();
  if (domain == NULL || signal <= 0 || signal >= (int)NSIG || sigset_size != 8) { return -EINVAL; }
  if (old_addr != 0) {
    struct k_sigaction64 old = {
      .handler = domain->signal_actions[signal].handler,
      .flags = domain->signal_actions[signal].flags,
      .restorer = domain->signal_actions[signal].restorer,
    };
    old.mask[0] = (uint32_t)(domain->signal_actions[signal].mask & 0xffffffffu);
    old.mask[1] = (uint32_t)(domain->signal_actions[signal].mask >> 32);
    if (!cell_domain_ensure_user_range(domain, old_addr, sizeof(old), VMM_ACCESS_WRITE) ||
        !vmm_copy_to_user(cell_domain_as(domain), old_addr, &old, sizeof(old))) {
      return -EFAULT;
    }
  }
  if (act_addr != 0) {
    struct k_sigaction64 act;
    if (!cell_domain_ensure_user_range(domain, act_addr, sizeof(act), VMM_ACCESS_READ) ||
        !vmm_copy_from_user(cell_domain_as(domain), &act, act_addr, sizeof(act))) {
      return -EFAULT;
    }
    domain->signal_actions[signal].handler = act.handler;
    domain->signal_actions[signal].flags = act.flags;
    domain->signal_actions[signal].restorer = act.restorer;
    domain->signal_actions[signal].mask = ((uint64_t)act.mask[1] << 32) | act.mask[0];
  }
  return 0;
}

int cell_rt_sigreturn(struct trap_frame *frame) {
  struct domain *domain = current_domain();
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
  return 0;
}

void cell_dump_current_fault(const struct trap_frame *frame, uint64_t far) {
  struct domain *domain = current_domain();
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
    for (size_t i = 0; i < vma_capacity(cell_domain_vmas(domain)); ++i) {
      const struct vma *vma = vma_at(cell_domain_vmas(domain), i);
      if (vma == NULL || !vma->used) { continue; }
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

int cell_kill(int pid, int signal) {
  if (pid == 0 || pid < -1) {
    int pgrp = pid == 0 ? cell_getpgid(0) : -pid;
    int delivered = 0;
    for (size_t i = 0; i < MAX_DOMAINS; ++i) {
      struct domain *domain = cell_domain_slot(i);
      if (domain != NULL && domain->used && !domain->zombie && domain->pgrp_id == pgrp) {
        if (signal == 0) {
          ++delivered;
          continue;
        }
        if (!signal_is_supported(signal)) {
          ++delivered;
          continue;
        }
        (void)cell_deliver_signal_to_thread(cell_thread_for_domain(domain), signal);
        ++delivered;
      }
    }
    return delivered == 0 ? -ESRCH : 0;
  }
  struct domain *domain = cell_find_domain(pid);
  if (domain == NULL || domain->zombie) { return -ESRCH; }
  if (signal == 0) { return 0; }
  if (!signal_is_supported(signal)) { return 0; }
  (void)cell_deliver_signal_to_thread(cell_thread_for_domain(domain), signal);
  return 0;
}

int cell_tkill(int tid, int signal) {
  if (signal == 0) {
    for (size_t i = 0; i < MAX_THREADS; ++i) {
      struct thread *thread = cell_thread_slot(i);
      if (thread != NULL && thread->state != THREAD_UNUSED && thread->tid == tid) { return 0; }
    }
    return -ESRCH;
  }
  if (!signal_is_supported(signal)) { return 0; }
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state == THREAD_UNUSED || thread->tid != tid || thread->domain == NULL) { continue; }
    (void)cell_deliver_signal_to_thread(thread, signal);
    return 0;
  }
  return -ESRCH;
}

int cell_tgkill(int pid, int tid, int signal) {
  struct domain *domain = cell_find_domain(pid);
  if (domain == NULL || domain->zombie) { return -ESRCH; }
  if (signal == 0) {
    for (size_t i = 0; i < MAX_THREADS; ++i) {
      struct thread *thread = cell_thread_slot(i);
      if (thread != NULL && thread->state != THREAD_UNUSED && thread->tid == tid && thread->domain == domain) {
        return 0;
      }
    }
    return -ESRCH;
  }
  if (!signal_is_supported(signal)) { return 0; }
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state == THREAD_UNUSED || thread->tid != tid || thread->domain != domain) {
      continue;
    }
    (void)cell_deliver_signal_to_thread(thread, signal);
    return 0;
  }
  return -ESRCH;
}
