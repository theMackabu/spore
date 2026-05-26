#pragma once

#include "cell.h"

int cell_alloc_pipe_obj(uint64_t fifo_ino);
void cell_pipe_free(uint8_t pipe_id);
void cell_pipe_reset(void);
int cell_fifo_pipe_for_ino(uint64_t ino, bool create);
bool cell_pipe_has_readers(uint8_t pipe_id);
void cell_pipe_add_reader(uint8_t pipe_id);
void cell_pipe_add_writer(uint8_t pipe_id);
void cell_pipe_drop_reader(uint8_t pipe_id);
void cell_pipe_drop_writer(uint8_t pipe_id);
bool cell_pipe_file_readable(struct open_file *file);
bool cell_pipe_file_writable(struct open_file *file);
bool cell_pipe_id_readable(uint8_t pipe_id);
bool cell_pipe_id_writable(uint8_t pipe_id);
bool cell_pipe_release_file(struct open_file *file);
int64_t cell_pipe_write_id_from_domain(struct domain *domain, uint8_t pipe_id, uint64_t buf, uint64_t len);
int64_t cell_pipe_read_id_to_domain(struct domain *domain, uint8_t pipe_id, uint64_t buf, uint64_t len);
int64_t cell_pipe_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
int64_t cell_pipe_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
void cell_pipe_notify(void);
