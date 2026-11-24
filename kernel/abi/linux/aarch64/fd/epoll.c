#include "handlers.h"
#include "abi/linux/aarch64/layout.h"
#include "abi/linux/shared/usercopy.h"
#include "proc/poll.h"

constexpr int EFAULT = 14;

int64_t sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t event_addr) {
  uint32_t events = 0;
  uint64_t data = 0;
  if ((int)op != 2) {
    struct epoll_event64 event;
    if (!syscall_user_readable(event_addr, sizeof(event)) ||
        !vmm_copy_from_user(syscall_active_as(), &event, event_addr, sizeof(event))) {
      return -(int64_t)EFAULT;
    }
    events = event.events;
    data = event.data;
  }
  return cell_fd_epoll_ctl((int)epfd, (int)op, (int)fd, events, data);
}
