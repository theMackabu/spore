#pragma once

#include <stdbool.h>
#include <stdint.h>

struct domain;
struct trap_frame;

void cell_tty_reset(void);
void cell_tty_process_input(void);
bool cell_tty_stdin_readable(void);
int cell_tty_pending_signal(void);
int cell_tty_take_pending_signal(void);
int64_t cell_tty_read_to_user(struct domain *domain, uint64_t buf, uint64_t len);
int64_t cell_tty_write_console_from_user(struct domain *domain, uint64_t buf, uint64_t len);
int cell_tty_foreground_pgrp(void);
int cell_tty_set_foreground_pgrp(int pgid);
uint32_t cell_tty_oflag(void);
void cell_tty_set_oflag(uint32_t oflag);
uint32_t cell_tty_lflag(void);
void cell_tty_set_lflag(uint32_t lflag);
uint8_t cell_tty_erase_char(void);
void cell_tty_set_erase_char(uint8_t ch);
void cell_wake_stdin(struct trap_frame *frame);
