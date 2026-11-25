#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned char *page;
static volatile sig_atomic_t handled;

static void recover(int signal, siginfo_t *info, void *context) {
  (void)context;
  if (signal != SIGSEGV || info->si_code != SEGV_ACCERR || info->si_addr != page + 31 ||
      mprotect(page, 4096, PROT_READ | PROT_WRITE)) { _exit(1); }
  ++handled;
}

int main(void) {
  page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) { return 2; }
  *(volatile unsigned char *)(page + 31) = 1;
  struct sigaction action = {.sa_sigaction = recover, .sa_flags = SA_SIGINFO};
  if (sigaction(SIGSEGV, &action, NULL) || mprotect(page, 4096, PROT_READ)) { return 3; }
  *(volatile unsigned char *)(page + 31) = 42;
  return handled == 1 ? page[31] : 4;
}
