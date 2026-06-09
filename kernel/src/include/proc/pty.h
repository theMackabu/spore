#pragma once

#include "cell.h"

#include <stdbool.h>
#include <stdint.h>

enum { CELL_PTY_CAP = 16 };

int cell_pty_open_master(uint32_t flags, const char *path);
int cell_pty_open_slave(int id, uint32_t flags, const char *path);
int cell_pty_open_peer(struct open_file *master, uint32_t flags);
int cell_pty_id_from_path(const char *path);
void cell_pty_release_file(struct open_file *file);
bool cell_pty_file_readable(const struct open_file *file);
bool cell_pty_file_writable(const struct open_file *file);
bool cell_pty_file_hup(const struct open_file *file);
uint64_t cell_pty_read_available(const struct open_file *file);
uint64_t cell_pty_write_pending(const struct open_file *file);
int cell_pty_id(const struct open_file *file);
bool cell_pty_is_master(const struct open_file *file);
bool cell_pty_unlocked(const struct open_file *file);
int cell_pty_set_locked(struct open_file *file, int locked);
uint32_t cell_pty_oflag(const struct open_file *file);
void cell_pty_set_oflag(struct open_file *file, uint32_t oflag);
uint32_t cell_pty_lflag(const struct open_file *file);
void cell_pty_set_lflag(struct open_file *file, uint32_t lflag);
uint8_t cell_pty_erase_char(const struct open_file *file);
void cell_pty_set_erase_char(struct open_file *file, uint8_t ch);
void cell_pty_get_winsize(const struct open_file *file, uint16_t *rows, uint16_t *cols);
void cell_pty_set_winsize(struct open_file *file, uint16_t rows, uint16_t cols);
int cell_pty_foreground_pgrp(const struct open_file *file);
int cell_pty_set_foreground_pgrp(struct open_file *file, int pgid);
int64_t cell_pty_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
int64_t cell_pty_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
int cell_block_current_on_pty(int fd, uint64_t buf, uint64_t len, bool write, struct trap_frame *frame);
