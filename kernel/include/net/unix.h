#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vfs_node;
struct domain;
struct open_file;
struct pipe_obj;
struct unix_rights;

constexpr uint8_t UNIX_DGRAM_PROTOCOL = 255;
int unix_dgram_init(struct open_file *file);
void unix_dgram_close(struct open_file *file);
int unix_dgram_bind(struct open_file *file, const char *path, const char *name);
const char *unix_dgram_name(const struct open_file *file, bool peer);
int unix_dgram_connect(struct open_file *file, const struct vfs_node *node);
int64_t unix_dgram_send(struct open_file *file, const struct vfs_node *node, const void *data, size_t len);

constexpr unsigned UNIX_MAX_RIGHTS = 253;

struct unix_rights_queue {
  struct unix_rights *head, *tail;
};

int unix_prepare_rights(struct domain *domain, const int32_t *fds, unsigned count, struct unix_rights **out);
void unix_release_rights(struct unix_rights *rights);
void unix_queue_rights(struct unix_rights_queue *queue, struct unix_rights *rights, uint64_t start, uint64_t end);
void unix_discard_rights(struct unix_rights_queue *queue, uint64_t consumed);
void unix_clear_rights(struct unix_rights_queue *queue);
const struct unix_rights *unix_next_rights(const struct unix_rights_queue *queue, uint64_t offset, uint64_t *bytes);
unsigned unix_install_rights(struct domain *domain, const struct unix_rights *rights, int *fds, unsigned capacity,
                             bool cloexec, bool *truncated);

void unix_track_socket(struct open_file *file);
void unix_untrack_socket(struct open_file *file);
void unix_check_last_reference(struct open_file *file);
bool unix_collect_cycles(void);
