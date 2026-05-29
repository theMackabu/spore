#include "arch/aarch64/syscall_handlers.h"

#include "cell.h"
#include "mem.h"
#include "pl011.h"

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
  FIONBIO = 0x5421,
  NCCS = 32,
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

int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
  if (request == TIOCGPGRP) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    int pgrp = cell_tty_foreground_pgrp();
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
    int rc = cell_tty_set_foreground_pgrp(pgrp);
    return rc == 0 ? 0 : (int64_t)rc;
  }
  if (request == TIOCGWINSZ) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    uint16_t rows = 0;
    uint16_t cols = 0;
    pl011_get_winsize(&rows, &cols);
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
  if (request == TIOCSWINSZ) { return require_tty_fd(fd); }
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
  if (request == TCGETS) {
    int64_t tty = require_tty_fd(fd);
    if (tty != 0) { return tty; }
    struct termios64 tio = {
      .c_iflag = 0,
      .c_oflag = 0,
      .c_cflag = 0,
      .c_lflag = cell_tty_lflag(),
      .c_line = 0,
      .c_cc = {0},
      .c_ispeed = 38400,
      .c_ospeed = 38400,
    };
    tio.c_cc[0] = 3;
    tio.c_cc[2] = cell_tty_erase_char();
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
    cell_tty_set_lflag(tio.c_lflag);
    cell_tty_set_erase_char(tio.c_cc[2]);
    return 0;
  }
  cell_note_unsupported_ioctl(request);
  return -(int64_t)EINVAL;
}
