#include "integration_socket.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int unix_peer_close_poll_regression(void) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) < 0) {
    printf("[spore] unix peer close poll: FAIL socketpair\n");
    return 0;
  }
  struct pollfd pfd = {.fd = pair[0], .events = POLLIN | POLLOUT};
  int ok = poll(&pfd, 1, 0) == 1 && pfd.revents == POLLOUT && write(pair[1], "x", 1) == 1;
  close(pair[1]);

  pfd.events = 0;
  ok = ok && poll(&pfd, 1, 0) == 1 && pfd.revents == POLLHUP;
  pfd.events = POLLIN | POLLOUT;
  char byte;
  ok = ok && poll(&pfd, 1, 0) == 1 && pfd.revents == (POLLIN | POLLHUP) &&
       read(pair[0], &byte, 1) == 1 && byte == 'x' &&
       poll(&pfd, 1, 0) == 1 && pfd.revents == (POLLIN | POLLHUP) && read(pair[0], &byte, 1) == 0;
  close(pair[0]);
  printf("[spore] unix peer close poll: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int unix_listener_names(void) {
  int cwd = open(".", O_RDONLY | O_DIRECTORY);
  if (cwd < 0 || chdir("/tmp") < 0) { return 0; }
  struct sockaddr_un old = {.sun_family = AF_UNIX, .sun_path = "spore-listen"};
  struct sockaddr_un moved = {.sun_family = AF_UNIX, .sun_path = "spore-listen-new"};
  int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  int replacement = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  int client = socket(AF_UNIX, SOCK_STREAM, 0), first = -1, second = -1;
  int ok = bind(listener, (void *)&old, sizeof(old)) == 0 && listen(listener, 4) == 0 &&
           rename(old.sun_path, moved.sun_path) == 0 && bind(replacement, (void *)&old, sizeof(old)) == 0 &&
           listen(replacement, 4) == 0 && connect(client, (void *)&old, sizeof(old)) == 0 &&
           (first = accept(replacement, NULL, NULL)) >= 0 && accept(listener, NULL, NULL) < 0 && errno == EAGAIN;
  close(client);
  client = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un alias = {.sun_family = AF_UNIX, .sun_path = "/socket-test-alias"};
  ok = ok && symlink("tmp/spore-listen-new", alias.sun_path) == 0 &&
       connect(client, (void *)&alias, sizeof(alias)) == 0 && (second = accept(listener, NULL, NULL)) >= 0;
  if (first >= 0) { close(first); }
  if (second >= 0) { close(second); }
  close(client); close(replacement); close(listener);
  unlink(old.sun_path);
  unlink(moved.sun_path);
  unlink(alias.sun_path);
  ok = fchdir(cwd) == 0 && ok;
  close(cwd);
  printf("[spore] Unix listener rename and rebind: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static ssize_t send_pair(int fd, const int files[2]) {
  union { struct cmsghdr align; unsigned char data[CMSG_SPACE(2 * sizeof(int))]; } control = {0};
  struct iovec iov[] = {{"a", 1}, {"b", 1}};
  struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2, .msg_control = control.data, .msg_controllen = sizeof(control)};
  struct cmsghdr *header = CMSG_FIRSTHDR(&msg);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(2 * sizeof(int));
  memcpy(CMSG_DATA(header), files, 2 * sizeof(int));
  return sendmsg(fd, &msg, MSG_NOSIGNAL);
}

static int receive_pair(int fd, int flags) {
  union { struct cmsghdr align; unsigned char data[CMSG_SPACE(2 * sizeof(int))]; } control = {0};
  char data[2];
  struct iovec iov[] = {{data, 1}, {data + 1, 1}};
  struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2, .msg_control = control.data, .msg_controllen = sizeof(control)};
  if (recvmsg(fd, &msg, flags | MSG_CMSG_CLOEXEC) != 2 || memcmp(data, "ab", 2)) { return 0; }
  struct cmsghdr *header = CMSG_FIRSTHDR(&msg);
  if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
      header->cmsg_len != CMSG_LEN(2 * sizeof(int)) || (msg.msg_flags & MSG_CTRUNC)) { return 0; }
  int files[2];
  memcpy(files, CMSG_DATA(header), sizeof(files));
  int ok = fcntl(files[0], F_GETFD) == FD_CLOEXEC && fcntl(files[1], F_GETFD) == FD_CLOEXEC &&
           write(files[1], "z", 1) == 1 && read(files[0], data, 1) == 1 && data[0] == 'z';
  close(files[0]); close(files[1]);
  return ok;
}

static int unix_rights_regression(void) {
  int pair[2], files[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) || pipe(files)) { return 0; }
  int invalid[] = {files[0], -1};
  char byte;
  int ok = send_pair(pair[1], invalid) < 0 && errno == EBADF && recv(pair[0], &byte, 1, MSG_DONTWAIT) < 0 &&
           errno == EAGAIN && send_pair(pair[1], files) == 2 && receive_pair(pair[0], MSG_PEEK) &&
           receive_pair(pair[0], 0);
  pid_t child = ok ? fork() : -1;
  if (!child) {
    close(pair[0]);
    usleep(30000);
    _exit(send_pair(pair[1], files) == 2 ? 0 : 1);
  }
  close(files[0]); close(files[1]); close(pair[1]);
  int status;
  ok = child > 0 && receive_pair(pair[0], 0) && waitpid(child, &status, 0) == child && status == 0;
  close(pair[0]);
  printf("[spore] Unix descriptor passing: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int unix_rights_lifetime(void) {
  int a[2], b[2], probe[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, a) || socketpair(AF_UNIX, SOCK_STREAM, 0, b) || pipe(probe)) { return 0; }
  int to_a[] = {b[0], probe[1]}, to_b[] = {a[0], probe[1]};
  int ok = send_pair(a[1], to_a) == 2 && send_pair(b[1], to_b) == 2;
  close(probe[1]); close(b[0]); close(b[1]); close(a[1]);
  struct pollfd watch = {.fd = probe[0], .events = POLLIN};
  ok = ok && poll(&watch, 1, 30) == 0;
  close(a[0]);
  char byte;
  ok = ok && poll(&watch, 1, 1000) == 1 && (watch.revents & POLLHUP) && read(probe[0], &byte, 1) == 0;
  close(probe[0]);
  printf("[spore] Unix in-flight descriptor lifetime: %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

static int unix_sequenced_packets(void) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair)) { return 0; }
  struct iovec parts[] = {{.iov_base = "ab", .iov_len = 2}, {.iov_base = "cd", .iov_len = 2}};
  char buffer[8] = {0};
  int type = 0;
  socklen_t typelen = sizeof(type);
  bool ok = getsockopt(pair[0], SOL_SOCKET, SO_TYPE, &type, &typelen) == 0 && type == SOCK_SEQPACKET &&
            writev(pair[0], parts, 2) == 4 && write(pair[0], "ef", 2) == 2 &&
            recv(pair[1], buffer, 1, MSG_PEEK | MSG_TRUNC) == 4 && *buffer == 'a';
  struct iovec small = {.iov_base = buffer, .iov_len = 2};
  struct msghdr msg = {.msg_iov = &small, .msg_iovlen = 1};
  ok = ok && recvmsg(pair[1], &msg, 0) == 2 && (msg.msg_flags & MSG_TRUNC) && !memcmp(buffer, "ab", 2);
  parts[0] = (struct iovec){.iov_base = buffer, .iov_len = 1};
  parts[1] = (struct iovec){.iov_base = buffer + 1, .iov_len = 4};
  ok = ok && readv(pair[1], parts, 2) == 2 && !memcmp(buffer, "ef", 2) && send(pair[0], "", 0, 0) == 0 &&
       recv(pair[1], buffer, sizeof(buffer), MSG_PEEK) == 0 && recv(pair[1], buffer, sizeof(buffer), 0) == 0 &&
       recv(pair[1], buffer, sizeof(buffer), 0) == -1 && errno == EAGAIN;
  close(pair[0]);
  ok = ok && read(pair[1], buffer, sizeof(buffer)) == 0;
  close(pair[1]);
  return ok;
}

int integration_unix_suite(void) {
  return unix_sequenced_packets() && unix_listener_names() && unix_peer_close_poll_regression() &&
         unix_rights_regression() && unix_rights_lifetime();
}
