#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mem.h"
#include "pl011.h"
#include "proc/domain.h"
#include "proc/pty.h"

#include <stdint.h>

enum {
  EFAULT = 14,
  EINVAL = 22,
  ENOTTY = 25,
  O_NONBLOCK = 04000,
  TCGETS = 0x5401,
  TCSETS = 0x5402,
  TCSETSW = 0x5403,
  TCSETSF = 0x5404,
  TIOCGWINSZ = 0x5413,
  TIOCSWINSZ = 0x5414,
  TIOCGPGRP = 0x540F,
  TIOCSPGRP = 0x5410,
  TIOCSCTTY = 0x540E,
  TIOCGPTN = 0x80045430,
  TIOCSPTLCK = 0x40045431,
  TIOCGPTLCK = 0x80045439,
  TIOCGPTPEER = 0x5441,
  TIOCOUTQ = 0x5411,
  TIOCSTI = 0x5412,
  FIONREAD = 0x541B,
  TIOCPKT = 0x5420,
  TIOCNOTTY = 0x5422,
  TIOCGSID = 0x5429,
  FIONBIO = 0x5421,
  FIONCLEX = 0x5450,
  FIOCLEX = 0x5451,
  FD_CLOEXEC = 1,
  NCCS = 32,
  I32_MAX = 2147483647,
};

struct termios64 {
  uint32_t c_iflag;
  uint32_t c_oflag;
  uint32_t c_cflag;
  uint32_t c_lflag;
  uint8_t c_line;
  uint8_t c_cc[NCCS];
  uint32_t c_ispeed;
  uint32_t c_ospeed;
};

struct winsize64 {
  uint16_t ws_row;
  uint16_t ws_col;
  uint16_t ws_xpixel;
  uint16_t ws_ypixel;
};

static int64_t require_tty_fd(uint64_t fd) {
  int tty = cell_fd_is_tty((int)fd);
  if (tty < 0) { return tty; }
  return tty != 0 ? 0 : -(int64_t)ENOTTY;
}

static struct open_file *fd_file(uint64_t fd) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || fd >= MAX_FDS) { return NULL; }
  return domain->fds[fd];
}

int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
  if (request == TIOCGPTN) {
    struct open_file *file = fd_file(fd);
    if (file == NULL || !cell_pty_is_master(file)) { return -(int64_t)ENOTTY; }
    int id = cell_pty_id(file);
    return syscall_user_writable(arg, sizeof(id)) && vmm_copy_to_user(syscall_active_as(), arg, &id, sizeof(id))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (request == TIOCSPTLCK) {
    struct open_file *file = fd_file(fd);
    if (file == NULL || !cell_pty_is_master(file)) { return -(int64_t)ENOTTY; }
    int locked = 0;
    if (!syscall_user_readable(arg, sizeof(locked)) ||
        !vmm_copy_from_user(syscall_active_as(), &locked, arg, sizeof(locked))) {
      return -(int64_t)EFAULT;
    }
    int rc = cell_pty_set_locked(file, locked);
    return rc == 0 ? 0 : (int64_t)rc;
  }
  if (request == TIOCGPTLCK) {
    struct open_file *file = fd_file(fd);
    if (file == NULL || !cell_pty_is_master(file)) { return -(int64_t)ENOTTY; }
    int locked = cell_pty_unlocked(file) ? 0 : 1;
    return syscall_user_writable(arg, sizeof(locked)) &&
               vmm_copy_to_user(syscall_active_as(), arg, &locked, sizeof(locked))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (request == TIOCGPTPEER) {
    struct open_file *file = fd_file(fd);
    if (file == NULL || !cell_pty_is_master(file)) { return -(int64_t)ENOTTY; }
    return cell_pty_open_peer(file, (uint32_t)arg);
  }
  if (request == FIONREAD || request == TIOCOUTQ) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    struct open_file *file = fd_file(fd);
    int queued = 0;
    if (file != NULL && file->type == OPEN_PTY) {
      uint64_t value = request == FIONREAD ? cell_pty_read_available(file) : cell_pty_write_pending(file);
      queued = value > I32_MAX ? I32_MAX : (int)value;
    }
    return syscall_user_writable(arg, sizeof(queued)) &&
               vmm_copy_to_user(syscall_active_as(), arg, &queued, sizeof(queued))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (request == TIOCGSID) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    struct domain *domain = cell_current_domain_internal();
    int sid = domain == NULL ? 1 : domain->session_id;
    return syscall_user_writable(arg, sizeof(sid)) && vmm_copy_to_user(syscall_active_as(), arg, &sid, sizeof(sid))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (request == TIOCNOTTY || request == TIOCPKT) { return require_tty_fd(fd); }
  if (request == TIOCSTI) { return -(int64_t)EINVAL; }
  if (request == TIOCSCTTY) { return require_tty_fd(fd); }
  if (request == TIOCGPGRP) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    struct open_file *file = fd_file(fd);
    int pgrp = file != NULL && file->type == OPEN_PTY ? cell_pty_foreground_pgrp(file) : cell_tty_foreground_pgrp();
    return syscall_user_writable(arg, sizeof(pgrp)) && vmm_copy_to_user(syscall_active_as(), arg, &pgrp, sizeof(pgrp))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (request == TIOCSPGRP) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    int pgrp = 0;
    if (!syscall_user_readable(arg, sizeof(pgrp)) ||
        !vmm_copy_from_user(syscall_active_as(), &pgrp, arg, sizeof(pgrp))) {
      return -(int64_t)EFAULT;
    }
    struct open_file *file = fd_file(fd);
    int rc = file != NULL && file->type == OPEN_PTY ? cell_pty_set_foreground_pgrp(file, pgrp)
                                                    : cell_tty_set_foreground_pgrp(pgrp);
    return rc == 0 ? 0 : (int64_t)rc;
  }
  if (request == TIOCGWINSZ) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    uint16_t rows = 0;
    uint16_t cols = 0;
    struct open_file *file = fd_file(fd);
    if (file != NULL && file->type == OPEN_PTY) {
      cell_pty_get_winsize(file, &rows, &cols);
    } else {
      pl011_get_winsize(&rows, &cols);
    }
    struct winsize64 ws = {
      .ws_row = rows,
      .ws_col = cols,
      .ws_xpixel = 0,
      .ws_ypixel = 0,
    };
    return syscall_user_writable(arg, sizeof(ws)) && vmm_copy_to_user(syscall_active_as(), arg, &ws, sizeof(ws))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (request == TIOCSWINSZ) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    struct winsize64 ws;
    if (!syscall_user_readable(arg, sizeof(ws)) || !vmm_copy_from_user(syscall_active_as(), &ws, arg, sizeof(ws))) {
      return -(int64_t)EFAULT;
    }
    struct open_file *file = fd_file(fd);
    if (file != NULL && file->type == OPEN_PTY) { cell_pty_set_winsize(file, ws.ws_row, ws.ws_col); }
    return 0;
  }
  if (request == FIONBIO) {
    int on = 0;
    if (!syscall_user_readable(arg, sizeof(on)) || !vmm_copy_from_user(syscall_active_as(), &on, arg, sizeof(on))) {
      return -(int64_t)EFAULT;
    }
    int flags = cell_fd_get_flags((int)fd);
    if (flags < 0) { return flags; }
    if (on != 0) {
      flags |= O_NONBLOCK;
    } else {
      flags &= ~O_NONBLOCK;
    }
    return cell_fd_set_flags((int)fd, flags);
  }
  if (request == FIOCLEX || request == FIONCLEX) {
    int flags = cell_fd_get_fd_flags((int)fd);
    if (flags < 0) { return flags; }
    if (request == FIOCLEX) {
      flags |= FD_CLOEXEC;
    } else {
      flags &= ~FD_CLOEXEC;
    }
    return cell_fd_set_fd_flags((int)fd, flags);
  }
  if (request == TCGETS) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    struct open_file *file = fd_file(fd);
    struct termios64 tio = {
      .c_iflag = 0,
      .c_oflag = 0,
      .c_cflag = 0,
      .c_lflag = file != NULL && file->type == OPEN_PTY ? cell_pty_lflag(file) : cell_tty_lflag(),
      .c_line = 0,
      .c_cc = {0},
      .c_ispeed = 38400,
      .c_ospeed = 38400,
    };
    tio.c_cc[0] = 3;
    tio.c_cc[2] = file != NULL && file->type == OPEN_PTY ? cell_pty_erase_char(file) : cell_tty_erase_char();
    tio.c_cc[3] = 21;
    tio.c_cc[4] = 4;
    tio.c_cc[5] = 0;
    tio.c_cc[6] = 1;
    return syscall_user_writable(arg, sizeof(tio)) && vmm_copy_to_user(syscall_active_as(), arg, &tio, sizeof(tio))
             ? 0
             : -(int64_t)EFAULT;
  }
  if (request == TCSETS || request == TCSETSW || request == TCSETSF) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    struct termios64 tio;
    if (!syscall_user_readable(arg, sizeof(tio)) || !vmm_copy_from_user(syscall_active_as(), &tio, arg, sizeof(tio))) {
      return -(int64_t)EFAULT;
    }
    struct open_file *file = fd_file(fd);
    if (file != NULL && file->type == OPEN_PTY) {
      cell_pty_set_lflag(file, tio.c_lflag);
      cell_pty_set_erase_char(file, tio.c_cc[2]);
    } else {
      cell_tty_set_lflag(tio.c_lflag);
      cell_tty_set_erase_char(tio.c_cc[2]);
    }
    return 0;
  }
  cell_note_unsupported_ioctl(request);
  return -(int64_t)EINVAL;
}
