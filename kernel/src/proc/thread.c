#include "proc/thread.h"

#include "arch/aarch64/smp.h"
#include "kprintf.h"
#include "mem.h"
#include "mm/vmm.h"
#include "proc/domain.h"

extern void arch_fp_save(struct fp_state *state);
extern void arch_fp_restore(const struct fp_state *state);

enum {
  EINVAL = 22,
};

static struct thread threads[MAX_THREADS];
static int next_thread_id = 1;

static void poweroff(void) {
  __asm__ volatile("mov x0, #0x0008\n"
                   "movk x0, #0x8400, lsl #16\n"
                   "hvc #0\n"
                   :
                   :
                   : "x0", "memory");
}

void cell_thread_reset(void) {
  kmemset(threads, 0, sizeof(threads));
  next_thread_id = 1;
}

struct domain *cell_current_domain_internal(void) {
  struct thread *current_thread = cell_current_thread_internal();
  return current_thread == NULL ? NULL : current_thread->domain;
}

struct thread *cell_current_thread_internal(void) {
  uint32_t cpu = smp_current_cpu();
  return smp_current_thread_slot(cpu);
}

void cell_set_current_thread(struct thread *thread) {
  uint32_t cpu = smp_current_cpu();
  smp_set_current_thread_slot(cpu, thread);
  if (thread != NULL) { thread->running_cpu = (int)cpu; }
}

bool cell_scheduler_waiting_for_interrupt(void) {
  return smp_scheduler_waiting(smp_current_cpu());
}

struct thread *cell_thread_slot(size_t index) {
  return index < MAX_THREADS ? &threads[index] : NULL;
}

size_t cell_thread_index(const struct thread *thread) {
  return thread == NULL ? MAX_THREADS : (size_t)(thread - threads);
}

struct thread *cell_alloc_thread(struct domain *domain) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].state == THREAD_UNUSED) {
      kmemset(&threads[i], 0, sizeof(threads[i]));
      threads[i].tid = next_thread_id++;
      threads[i].running_cpu = -1;
      threads[i].domain = domain;
      threads[i].wait_reason = WAIT_NONE;
      threads[i].wait_target = -1;
      ++domain->refcount;
      return &threads[i];
    }
  }
  return NULL;
}

void cell_release_thread(struct thread *thread) {
  if (thread == NULL || thread->domain == NULL) { return; }
  struct domain *domain = thread->domain;
  if (domain->refcount > 0) { --domain->refcount; }
  thread->state = THREAD_UNUSED;
  thread->running_cpu = -1;
  thread->domain = NULL;
  for (uint32_t cpu = 0; cpu < smp_present_cpu_count(); ++cpu) {
    smp_clear_current_thread_if(cpu, thread);
  }
}

struct thread *cell_thread_for_domain(struct domain *domain) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].state != THREAD_UNUSED && threads[i].domain == domain) { return &threads[i]; }
  }
  return NULL;
}

size_t cell_runnable_or_blocked_threads_in_domain(const struct domain *domain) {
  size_t count = 0;
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    if (threads[i].domain == domain && (threads[i].state == THREAD_RUNNABLE || threads[i].state == THREAD_BLOCKED)) {
      ++count;
    }
  }
  return count;
}

void cell_wake_vfork_parent_of(int child_id) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL) { continue; }
    if (thread->state == THREAD_BLOCKED && thread->wait_reason == WAIT_VFORK && thread->wait_target == child_id) {
      thread->state = THREAD_RUNNABLE;
      thread->wait_reason = WAIT_NONE;
      thread->wait_target = -1;
    }
  }
}

int cell_sleep_current(uint64_t timeout_ticks, struct trap_frame *frame) {
  if (cell_current_thread_internal() == NULL || cell_current_domain_internal() == NULL) { return -EINVAL; }
  if (timeout_ticks == 0) { return 0; }
  return cell_block_current_on_sleep(cell_uptime_ticks() + timeout_ticks, frame);
}

void cell_wake_sleep_waiters(uint64_t scheduler_ticks) {
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread == NULL || thread->state != THREAD_BLOCKED || thread->wait_reason != WAIT_SLEEP) { continue; }
    if (scheduler_ticks < thread->sleep_deadline_tick) { continue; }
    thread->tf.x[0] = 0;
    thread->state = THREAD_RUNNABLE;
    thread->wait_reason = WAIT_NONE;
    thread->sleep_deadline_tick = 0;
  }
}

void cell_save_current(const struct trap_frame *frame) {
  struct thread *current_thread = cell_current_thread_internal();
  if (current_thread == NULL) { return; }
  current_thread->tf = *frame;
  arch_fp_save(&current_thread->fp);
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(current_thread->tpidr_el0));
}

static void restore_thread(struct thread *thread, struct trap_frame *frame, struct domain *old_domain) {
  (void)old_domain;
  vmm_install_user(cell_domain_as(thread->domain));
  arch_fp_restore(&thread->fp);
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(thread->tpidr_el0));
  *frame = thread->tf;
}

static void cleanup_reaped_current_domain(struct domain *old_domain, struct thread *next) {
  if (old_domain == NULL || old_domain == next->domain || !old_domain->zombie || old_domain->parent_id != 0) { return; }
  for (size_t i = 0; i < MAX_THREADS; ++i) {
    struct thread *thread = cell_thread_slot(i);
    if (thread != NULL && thread->domain == old_domain && thread->running_cpu >= 0) { return; }
  }
  cell_destroy_domain(old_domain);
}

void cell_restore_current(struct trap_frame *frame) {
  struct thread *current_thread = cell_current_thread_internal();
  if (current_thread == NULL) { return; }
  vmm_install_user(cell_domain_as(current_thread->domain));
  arch_fp_restore(&current_thread->fp);
  __asm__ volatile("msr tpidr_el0, %0" : : "r"(current_thread->tpidr_el0));
  *frame = current_thread->tf;
}

void cell_schedule(struct trap_frame *frame) {
  uint32_t cpu = smp_current_cpu();
  struct thread *current_thread = cell_current_thread_internal();
  struct domain *old_domain = cell_current_domain_internal();
  cell_save_current(frame);
  size_t start = current_thread == NULL ? 0 : cell_thread_index(current_thread) + 1;
  for (;;) {
    if (current_thread != NULL && current_thread->state == THREAD_RUNNABLE) {
      for (size_t n = 0; n < MAX_THREADS; ++n) {
        struct thread *candidate = cell_thread_slot((start + n) % MAX_THREADS);
        if (candidate != NULL && candidate->state == THREAD_RUNNABLE &&
            (candidate->running_cpu < 0 || candidate->running_cpu == (int)cpu)) {
          if (current_thread != NULL && current_thread != candidate && current_thread->running_cpu == (int)cpu) {
            current_thread->running_cpu = -1;
          }
          smp_set_current_thread_slot(cpu, candidate);
          candidate->running_cpu = (int)cpu;
          cleanup_reaped_current_domain(old_domain, candidate);
          restore_thread(candidate, frame, old_domain);
          return;
        }
      }
    }
    for (size_t n = 0; n < MAX_THREADS; ++n) {
      struct thread *candidate = cell_thread_slot((start + n) % MAX_THREADS);
      if (candidate != NULL && candidate->state == THREAD_RUNNABLE && candidate->running_cpu < 0) {
        if (current_thread != NULL && current_thread->running_cpu == (int)cpu) { current_thread->running_cpu = -1; }
        smp_set_current_thread_slot(cpu, candidate);
        candidate->running_cpu = (int)cpu;
        cleanup_reaped_current_domain(old_domain, candidate);
        restore_thread(candidate, frame, old_domain);
        return;
      }
    }

    bool has_blocked = false;
    bool has_running_elsewhere = false;
    for (size_t i = 0; i < MAX_THREADS; ++i) {
      struct thread *thread = cell_thread_slot(i);
      if (thread != NULL && thread->state == THREAD_BLOCKED) {
        has_blocked = true;
      }
      if (thread != NULL && thread->state == THREAD_RUNNABLE && thread->running_cpu >= 0 &&
          thread->running_cpu != (int)cpu) {
        has_running_elsewhere = true;
      }
    }
    if (!has_blocked && has_running_elsewhere) {
      smp_set_scheduler_waiting(cpu, true);
      smp_kernel_unlock();
      __asm__ volatile("msr daifclr, #2\n"
                       "wfi\n"
                       "msr daifset, #2\n"
                       :
                       :
                       : "memory");
      smp_kernel_lock();
      smp_set_scheduler_waiting(cpu, false);
      continue;
    }
    if (!has_blocked) { break; }
    smp_set_scheduler_waiting(cpu, true);
    smp_kernel_unlock();
    __asm__ volatile("msr daifclr, #2\n"
                     "wfi\n"
                     "msr daifset, #2\n"
                     :
                     :
                     : "memory");
    smp_kernel_lock();
    smp_set_scheduler_waiting(cpu, false);
  }
  kprintf("[kernel] no runnable threads\n");
  poweroff();
  for (;;) {
    __asm__ volatile("wfe");
  }
}

int cell_block_current_on_pipe(int fd, uint64_t buf, uint64_t len, bool write, struct trap_frame *frame) {
  struct thread *current_thread = cell_current_thread_internal();
  cell_save_current(frame);
  current_thread->running_cpu = -1;
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_PIPE;
  current_thread->wait_target = fd;
  current_thread->pipe_buf = buf;
  current_thread->pipe_len = len;
  current_thread->pipe_write = write;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_block_current_on_sleep(uint64_t deadline_tick, struct trap_frame *frame) {
  struct thread *current_thread = cell_current_thread_internal();
  cell_save_current(frame);
  current_thread->running_cpu = -1;
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_SLEEP;
  current_thread->sleep_deadline_tick = deadline_tick;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_block_current_on_socket(int fd, uint64_t buf, uint64_t len, uint64_t addr, uint64_t addrlen,
                                 struct trap_frame *frame) {
  return cell_block_current_on_socket_timeout(fd, buf, len, addr, addrlen, 0, frame);
}

int cell_block_current_on_socket_timeout(int fd, uint64_t buf, uint64_t len, uint64_t addr, uint64_t addrlen,
                                         uint64_t timeout_ticks, struct trap_frame *frame) {
  return cell_block_current_on_socket_flags_timeout(fd, buf, len, addr, addrlen, 0, timeout_ticks, frame);
}

int cell_block_current_on_socket_flags_timeout(int fd, uint64_t buf, uint64_t len, uint64_t addr, uint64_t addrlen,
                                               uint32_t flags, uint64_t timeout_ticks, struct trap_frame *frame) {
  struct thread *current_thread = cell_current_thread_internal();
  cell_save_current(frame);
  current_thread->running_cpu = -1;
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_SOCKET;
  current_thread->wait_target = fd;
  current_thread->pipe_buf = buf;
  current_thread->pipe_len = len;
  current_thread->socket_addr = addr;
  current_thread->socket_addrlen = addrlen;
  current_thread->socket_msg = false;
  current_thread->socket_write = false;
  current_thread->socket_msg_addr = 0;
  current_thread->socket_iov = 0;
  current_thread->socket_iovlen = 0;
  current_thread->socket_flags = flags;
  current_thread->socket_has_deadline = timeout_ticks != 0;
  current_thread->socket_deadline_tick = timeout_ticks == 0 ? 0 : cell_uptime_ticks() + timeout_ticks;
  current_thread->pipe_write = false;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_block_current_on_socket_write_timeout(int fd, uint64_t buf, uint64_t len, uint64_t timeout_ticks,
                                               struct trap_frame *frame) {
  struct thread *current_thread = cell_current_thread_internal();
  cell_save_current(frame);
  current_thread->running_cpu = -1;
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_SOCKET;
  current_thread->wait_target = fd;
  current_thread->pipe_buf = buf;
  current_thread->pipe_len = len;
  current_thread->socket_addr = 0;
  current_thread->socket_addrlen = 0;
  current_thread->socket_msg = false;
  current_thread->socket_write = true;
  current_thread->socket_msg_addr = 0;
  current_thread->socket_iov = 0;
  current_thread->socket_iovlen = 0;
  current_thread->socket_flags = 0;
  current_thread->socket_has_deadline = timeout_ticks != 0;
  current_thread->socket_deadline_tick = timeout_ticks == 0 ? 0 : cell_uptime_ticks() + timeout_ticks;
  current_thread->pipe_write = false;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_block_current_on_socket_sendmsg_timeout(int fd, uint64_t msg_addr, uint64_t timeout_ticks,
                                                 struct trap_frame *frame) {
  struct thread *current_thread = cell_current_thread_internal();
  cell_save_current(frame);
  current_thread->running_cpu = -1;
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_SOCKET;
  current_thread->wait_target = fd;
  current_thread->pipe_buf = 0;
  current_thread->pipe_len = 0;
  current_thread->socket_addr = 0;
  current_thread->socket_addrlen = 0;
  current_thread->socket_msg = true;
  current_thread->socket_write = true;
  current_thread->socket_msg_addr = msg_addr;
  current_thread->socket_iov = 0;
  current_thread->socket_iovlen = 0;
  current_thread->socket_flags = 0;
  current_thread->socket_has_deadline = timeout_ticks != 0;
  current_thread->socket_deadline_tick = timeout_ticks == 0 ? 0 : cell_uptime_ticks() + timeout_ticks;
  current_thread->pipe_write = false;
  cell_schedule(frame);
  return CELL_SWITCHED;
}

int cell_block_current_on_socket_msg(int fd, uint64_t msg_addr, uint64_t iov, uint64_t iovlen, uint64_t addr,
                                     uint64_t addrlen, uint32_t flags, struct trap_frame *frame) {
  return cell_block_current_on_socket_msg_timeout(fd, msg_addr, iov, iovlen, addr, addrlen, flags, 0, frame);
}

int cell_block_current_on_socket_msg_timeout(int fd, uint64_t msg_addr, uint64_t iov, uint64_t iovlen, uint64_t addr,
                                             uint64_t addrlen, uint32_t flags, uint64_t timeout_ticks,
                                             struct trap_frame *frame) {
  struct thread *current_thread = cell_current_thread_internal();
  cell_save_current(frame);
  current_thread->running_cpu = -1;
  current_thread->state = THREAD_BLOCKED;
  current_thread->wait_reason = WAIT_SOCKET;
  current_thread->wait_target = fd;
  current_thread->pipe_buf = 0;
  current_thread->pipe_len = 0;
  current_thread->socket_addr = addr;
  current_thread->socket_addrlen = addrlen;
  current_thread->socket_msg = true;
  current_thread->socket_write = false;
  current_thread->socket_msg_addr = msg_addr;
  current_thread->socket_iov = iov;
  current_thread->socket_iovlen = iovlen;
  current_thread->socket_flags = flags;
  current_thread->socket_has_deadline = timeout_ticks != 0;
  current_thread->socket_deadline_tick = timeout_ticks == 0 ? 0 : cell_uptime_ticks() + timeout_ticks;
  current_thread->pipe_write = false;
  cell_schedule(frame);
  return CELL_SWITCHED;
}
