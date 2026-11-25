#include "proc/seccomp.h"
#include "abi/linux/shared/usercopy.h"
#include "arch/regs.h"
#include "lib/mem.h"
#include "mm/pmm.h"
#include "proc/domain.h"
#include "proc/process.h"
#include "proc/signal.h"
#include "proc/thread.h"

enum {
  BPF_LD_W_ABS = 0x20,
  BPF_JA = 0x05,
  BPF_JEQ = 0x15,
  BPF_JGT = 0x25,
  BPF_JGE = 0x35,
  BPF_JSET = 0x45,
  BPF_AND = 0x54,
  BPF_RET_K = 0x06
};
constexpr uint32_t RET_ALLOW = 0x7fff0000, RET_ERRNO = 0x00050000, RET_KILL_THREAD = 0, RET_KILL_PROCESS = 0x80000000;
constexpr unsigned FILTER_MAX_INSNS = 4096, FILTER_MAX_PATH = 32768;

struct instruction {
  uint16_t code;
  uint8_t jt, jf;
  uint32_t k;
};
struct seccomp_data {
  int32_t nr;
  uint32_t arch;
  uint64_t ip, args[6];
};
static_assert(sizeof(struct seccomp_data) == 64);

struct seccomp_filter {
  struct seccomp_filter *previous;
  uint64_t pa;
  unsigned refs, length, pages, path_cost;
  struct instruction code[];
};

struct seccomp_filter *seccomp_inherit(struct seccomp_filter *filter) {
  if (filter) { ++filter->refs; }
  return filter;
}

void seccomp_release(struct seccomp_filter *filter) {
  while (filter && --filter->refs == 0) {
    struct seccomp_filter *previous = filter->previous;
    uint64_t pa = filter->pa;
    unsigned pages = filter->pages;
    for (unsigned i = 0; i < pages; ++i) {
      pmm_free_page(pa + i * PAGE_SIZE);
    }
    filter = previous;
  }
}

static bool valid_program(const struct seccomp_filter *filter) {
  if (filter->code[filter->length - 1].code != BPF_RET_K) { return false; }
  for (unsigned pc = 0; pc < filter->length; ++pc) {
    const struct instruction ins = filter->code[pc];
    unsigned remaining = filter->length - pc - 1;
    switch (ins.code) {
    case BPF_LD_W_ABS:
      if ((ins.k & 3) || ins.k >= sizeof(struct seccomp_data)) { return false; }
      break;
    case BPF_JEQ:
    case BPF_JGT:
    case BPF_JGE:
    case BPF_JSET:
      if (ins.jt >= remaining || ins.jf >= remaining) { return false; }
      break;
    case BPF_JA:
      if (ins.k >= remaining) { return false; }
      break;
    case BPF_RET_K: {
      uint32_t action = ins.k & 0xffff0000u;
      if (action != RET_ALLOW && action != RET_ERRNO && action != RET_KILL_THREAD && action != RET_KILL_PROCESS) {
        return false;
      }
      break;
    }
    case BPF_AND:
      break;
    default:
      return false;
    }
  }
  return true;
}

int64_t sys_seccomp(unsigned op, unsigned flags, uint64_t address) {
  constexpr unsigned set_mode_filter = 1, thread_sync = 1;
  if (op != set_mode_filter || flags & ~thread_sync) { return -22; }
  struct thread *thread = cell_current_thread_internal();
  struct domain *domain = thread->domain;
  if (!thread->no_new_privs && !cell_capable(CAP_SYS_ADMIN)) { return -13; }
  if (!(flags & thread_sync) && domain->refcount != 1) { return -16; }
  struct {
    uint16_t length;
    uint64_t program;
  } user;
  struct user_address_space *as = syscall_active_as();
  if (!syscall_user_readable(address, sizeof(user)) || !vmm_copy_from_user(as, &user, address, sizeof(user))) {
    return -14;
  }
  if (!user.length || user.length > FILTER_MAX_INSNS) { return -22; }
  unsigned cost = user.length + (domain->filter ? domain->filter->path_cost + 4 : 0);
  if (cost > FILTER_MAX_PATH) { return -12; }
  size_t bytes = sizeof(struct instruction) * user.length;
  if (!syscall_user_readable(user.program, bytes)) { return -14; }
  unsigned pages = (sizeof(struct seccomp_filter) + bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  uint64_t pa = pmm_alloc_contiguous_pages(pages);
  if (!pa) { return -12; }
  struct seccomp_filter *filter = pmm_phys_to_virt(pa);
  *filter = (struct seccomp_filter){.pa = pa, .refs = 1, .length = user.length, .pages = pages, .path_cost = cost};
  if (!vmm_copy_from_user(as, filter->code, user.program, bytes)) {
    seccomp_release(filter);
    return -14;
  }
  if (!valid_program(filter)) {
    seccomp_release(filter);
    return -22;
  }
  filter->previous = domain->filter;
  if ((flags & thread_sync) && thread->no_new_privs && domain->refcount > 1) {
    for (size_t i = 0; i < cell_thread_slot_limit(); ++i) {
      struct thread *sibling = cell_thread_slot(i);
      if (sibling && sibling->domain == domain) { sibling->no_new_privs = true; }
    }
  }
  domain->filter = filter;
  return 0;
}

static uint32_t evaluate(const struct seccomp_filter *filter, const struct seccomp_data *data) {
  uint32_t acc = 0;
  for (unsigned pc = 0; pc < filter->length; ++pc) {
    struct instruction ins = filter->code[pc];
    switch (ins.code) {
    case BPF_LD_W_ABS:
      kmemcpy(&acc, (const char *)data + ins.k, sizeof(acc));
      break;
    case BPF_AND:
      acc &= ins.k;
      break;
    case BPF_JA:
      pc += ins.k;
      break;
    case BPF_JEQ:
      pc += acc == ins.k ? ins.jt : ins.jf;
      break;
    case BPF_JGT:
      pc += acc > ins.k ? ins.jt : ins.jf;
      break;
    case BPF_JGE:
      pc += acc >= ins.k ? ins.jt : ins.jf;
      break;
    case BPF_JSET:
      pc += (acc & ins.k) ? ins.jt : ins.jf;
      break;
    case BPF_RET_K:
      return ins.k;
    default:
      return RET_KILL_PROCESS;
    }
  }
  return RET_KILL_PROCESS;
}

bool seccomp_check(struct trap_frame *frame, uint64_t nr, const uint64_t args[6], int64_t *result) {
  struct domain *domain = cell_current_domain_internal();
  if (!domain || !domain->filter) { return true; }
  struct seccomp_data data = {.nr = (int32_t)nr};
#if defined(__aarch64__) || defined(__wasm32__)
  data.arch = 0xc00000b7;
  data.ip = frame->elr_el1;
#elif defined(__x86_64__)
  data.arch = 0xc000003e;
  data.ip = frame->rip;
#else
  data.arch = 0xc00000f3;
  data.ip = frame->pc;
#endif
  kmemcpy(data.args, args, sizeof(data.args));
  uint32_t choice = RET_ALLOW;
  for (struct seccomp_filter *f = domain->filter; f; f = f->previous) {
    uint32_t value = evaluate(f, &data);
    uint32_t action = value & 0xffff0000u;
    if (action == RET_KILL_PROCESS) {
      cell_terminate_current(31, frame);
      *result = CELL_SWITCHED;
      return false;
    }
    if (action == RET_KILL_THREAD) { choice = RET_KILL_THREAD; }
    if (action == RET_ERRNO && choice == RET_ALLOW) { choice = value; }
  }
  if (choice == RET_ALLOW) { return true; }
  if (choice == RET_KILL_THREAD) {
    if (cell_runnable_or_blocked_threads_in_domain(domain) > 1) {
      cell_exit_thread_current(128 + 31, frame);
    } else {
      cell_terminate_current(31, frame);
    }
    *result = CELL_SWITCHED;
    return false;
  }
  unsigned error = choice & 0xffff;
  *result = -(int64_t)(error < 4095 ? error : 4095);
  return false;
}
