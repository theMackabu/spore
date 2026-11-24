#include "internal.h"
#include "net/unix.h"
#include "proc/domain.h"
#include "proc/poll.h"
#include "proc/thread.h"

static void wake_socket_timeout_waiters(uint64_t now_ticks) {
  size_t cursor = 0;
  struct thread *thread;
  while ((thread = cell_next_waiter(WAIT_SOCKET, &cursor))) {
    if (!thread->wait.has_deadline || now_ticks < thread->wait.deadline_tick) {
      continue;
    }
    cell_finish_wait(thread, -EAGAIN);
  }
}

static int64_t resume_socket_wait(struct open_file *file, struct thread *thread) {
  const struct thread_wait *wait = &thread->wait;
  struct domain *domain = thread->domain;
  if (file->type == OPEN_UNIX_LISTENER) { return take_pending_unix(domain, file, wait->socket_accept_flags); }
  if (wait->write) {
    return wait->socket_msg ? socket_sendmsg_tcp_from_domain(domain, file, wait->socket_msg_addr)
                            : cell_socket_tcp_write_from_domain(domain, file, wait->buf, wait->len);
  }
  if (wait->socket_msg) {
    return socket_recvmsg_to_iov(domain, file, wait->socket_msg_addr, wait->socket_iov, (int32_t)wait->socket_iovlen,
                                 wait->socket_addr, wait->socket_addrlen, wait->socket_flags);
  }
  if (file->socket_proto != IPPROTO_TCP) {
    return dgram_read_to_domain(domain, file, wait->buf, wait->len, wait->socket_addr, wait->socket_addrlen,
                                wait->socket_flags);
  }
  if (wait->buf || wait->len) {
    if (tcp_recv_waitall_pending(file, wait->len, wait->socket_flags)) { return -EAGAIN; }
    return cell_socket_tcp_read_to_domain_flags(domain, file, wait->buf, wait->len, wait->socket_flags);
  }
  if (file->tcp_state == TCP_LISTEN) {
    int accepted = cell_socket_take_pending_tcp(domain, file, wait->socket_accept_flags);
    if (accepted < 0) { return accepted; }
    int rc = tcp_copy_peer_addr_to_domain(domain, accepted, wait->socket_addr, wait->socket_addrlen);
    if (rc < 0) { cell_close_domain_fd(domain, accepted); return rc; }
    return accepted;
  }
  if (file->tcp_error) { return -(int)file->tcp_error; }
  return file->tcp_state == TCP_ESTABLISHED ? 0 : -EAGAIN;
}

void cell_socket_wake_file(struct open_file *file) {
  if (file->socket_waiters) {
    cell_retain_open_file(file);
    size_t cursor = 0;
    struct thread *thread;
    while ((thread = cell_next_waiter(WAIT_SOCKET, &cursor))) {
      if (thread->wait.held_file != file || thread->wait.socket_servicing) { continue; }
      thread->wait.socket_servicing = true;
      int64_t rc = resume_socket_wait(file, thread);
      thread->wait.socket_servicing = false;
      if (rc != -EAGAIN) { cell_finish_wait(thread, rc); }
    }
    cell_release_open_file(file);
  }
  cell_wake_poll_waiters_internal();
}

void cell_socket_timer_tick(uint64_t now_ticks) {
  wake_socket_timeout_waiters(now_ticks);
  tcp_timer_tick(now_ticks);
  (void)unix_collect_cycles();
}
