#pragma once

#include "cell.h"

void cell_tty_reset(void);
void cell_tty_process_input(void);
bool cell_tty_stdin_readable(void);
int cell_tty_pending_signal(void);
int cell_tty_take_pending_signal(void);
int64_t cell_tty_read_to_user(struct domain *domain, uint64_t buf, uint64_t len);
int64_t cell_tty_write_console_from_user(struct domain *domain, uint64_t buf, uint64_t len);
