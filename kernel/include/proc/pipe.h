#pragma once

#include "mm/page_ring.h"
#include <stdbool.h>
#include <stdint.h>

struct domain;
struct open_file;
struct pipe_obj;
struct vfs_node;

struct pipe_obj *cell_pipe_create(void);
struct pipe_obj *cell_fifo_open(const struct vfs_node *node, bool create);
void cell_pipe_discard(struct pipe_obj *pipe);
uint64_t cell_pipe_ino(const struct pipe_obj *pipe);
bool cell_pipe_has_readers(struct pipe_obj *pipe);
void cell_pipe_add_reader(struct pipe_obj *pipe);
void cell_pipe_add_writer(struct pipe_obj *pipe);
void cell_pipe_drop_reader(struct pipe_obj *pipe);
void cell_pipe_drop_writer(struct pipe_obj *pipe);
bool cell_pipe_file_readable(struct open_file *file);
bool cell_pipe_file_writable(struct open_file *file);
bool cell_pipe_file_hup(struct open_file *file);
bool cell_pipe_readable(struct pipe_obj *pipe);
bool cell_pipe_writable(struct pipe_obj *pipe);
bool cell_pipe_hup(struct pipe_obj *pipe);
struct unix_rights;
struct unix_rights_queue;
uint64_t cell_pipe_read_offset(const struct pipe_obj *pipe);
struct unix_rights_queue *cell_pipe_rights(struct pipe_obj *pipe);
int64_t cell_pipe_write_data(struct pipe_obj *pipe, uint64_t len, ring_copy_in copy, void *source, struct unix_rights **rights);
int64_t cell_pipe_peek_data(struct pipe_obj *pipe, uint64_t len, ring_copy_out copy, void *dst);
void cell_pipe_consume(struct pipe_obj *pipe, uint32_t bytes);
int64_t cell_pipe_recv_user(struct domain *domain, struct pipe_obj *pipe, uint64_t buf, uint64_t len, bool peek);
bool cell_pipe_release_file(struct open_file *file);
int64_t cell_pipe_write_user(struct domain *domain, struct pipe_obj *pipe, uint64_t buf, uint64_t len);
int64_t cell_pipe_read_user(struct domain *domain, struct pipe_obj *pipe, uint64_t buf, uint64_t len);
int64_t cell_pipe_write_from_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
int64_t cell_pipe_read_to_domain(struct domain *domain, struct open_file *file, uint64_t buf, uint64_t len);
void cell_pipe_notify(void);
