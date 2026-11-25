#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>

static void handler(int sig, siginfo_t *info, void *ptr) {
  ucontext_t *uc = ptr;
  if (sig != SIGTRAP || info->si_addr != (void *)uc->uc_mcontext.pc) { _Exit(1); }
  uint32_t *fp = (void *)uc->uc_mcontext.__reserved;
  if (fp[0] != 0x46508001 || fp[1] != 528 || fp[4] != 17) { _Exit(2); }
  fp[4] = 42;
  uc->uc_mcontext.pc += 4;
  __asm__ volatile("movi v0.16b, #0" ::: "v0");
}

int main(void) {
  struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
  if (sigaction(SIGTRAP, &sa, 0)) return 3;
  unsigned value;
  __asm__ volatile("mov w9, #17\n fmov s0,w9\n brk #3\n fmov %w0,s0" : "=r"(value)::"x9", "v0", "memory");
  return value;
}
