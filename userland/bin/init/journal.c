#include "mycelium.h"

pid_t fork_unit(struct unit *unit, struct journal_stream *stream) {
  bool out = streq(unit->config.standard_output, "journal");
  bool err = streq(unit->config.standard_error, "journal");
  int ends[2] = {-1, -1};
  if (out || err) {
    if (pipe(ends) < 0) { return -1; }
    if (fcntl(ends[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(ends[0], F_SETFD, FD_CLOEXEC) < 0) {
      close(ends[0]);
      close(ends[1]);
      return -1;
    }
  }
  pid_t pid = fork();
  if (pid == 0) {
    if (ends[0] >= 0) {
      close(ends[0]);
      if ((out && dup2(ends[1], 1) < 0) || (err && dup2(ends[1], 2) < 0)) { _exit(126); }
      close(ends[1]);
    }
    redirect_output(unit->config.standard_output, 1, 1);
    redirect_output(unit->config.standard_error, 2, 2);
  } else {
    if (ends[1] >= 0) { close(ends[1]); }
    if (pid < 0 && ends[0] >= 0) { close(ends[0]); }
    *stream = (struct journal_stream){.fd = pid < 0 ? -1 : ends[0]};
  }
  return pid;
}

static void emit(struct unit *unit, struct journal_stream *stream) {
  stream->line[stream->used] = 0;
  if (strstr(stream->line, "READY=1") && unit->state == STATE_ACTIVATING) { unit->state = STATE_ACTIVE; }
  if (strstr(stream->line, "WATCHDOG=1")) { unit->last_watchdog = time(NULL); }
  journal_append(unit, "%s", stream->line);
  stream->used = 0;
}

static ssize_t read_chunk(struct unit *unit, struct journal_stream *stream) {
  char bytes[512];
  ssize_t n;
  do {
    n = read(stream->fd, bytes, sizeof(bytes));
  } while (n < 0 && errno == EINTR);
  for (ssize_t i = 0; i < n; ++i) {
    if (bytes[i] == '\n') {
      emit(unit, stream);
      continue;
    }
    if (stream->used == sizeof(stream->line) - 1) { emit(unit, stream); }
    stream->line[stream->used++] = bytes[i] ? bytes[i] : '?';
  }
  return n;
}

static void finish(struct unit *unit, struct journal_stream *stream) {
  if (stream->used) { emit(unit, stream); }
  close(stream->fd);
  stream->fd = -1;
}

void journal_read(struct unit *unit, struct journal_stream *stream) {
  if (stream->fd < 0) { return; }
  ssize_t n = read_chunk(unit, stream);
  if (!n || (n < 0 && errno != EAGAIN)) { finish(unit, stream); }
}

void journal_finish(struct unit *unit, struct journal_stream *stream) {
  if (stream->fd < 0) { return; }
  int available = 0;
  if (ioctl(stream->fd, FIONREAD, &available) == 0) {
    while (available > 0) {
      ssize_t n = read_chunk(unit, stream);
      if (n <= 0) { break; }
      available -= n;
    }
  } else {
    while (read_chunk(unit, stream) > 0) {}
  }
  finish(unit, stream);
}
