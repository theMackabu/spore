#pragma once

#include "proc/limits.h"
#include "fs/vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;
struct trap_frame;
struct tcp_ooo_state;
struct tcp_segment;
struct tcp_rx_state;

enum open_file_type {
  OPEN_NONE,
  OPEN_STDIN,
  OPEN_STDOUT,
  OPEN_VFS,
  OPEN_SOCKET,
  OPEN_PIPE,
  OPEN_UNIX_STREAM,
  OPEN_UNIX_LISTENER,
  OPEN_EPOLL,
  OPEN_EVENTFD,
  OPEN_INOTIFY,
  OPEN_PTY,
};

constexpr int CELL_EPOLL_WATCH_CAP = 16;

struct epoll_watch {
  bool used;
  int32_t fd;
  uint32_t events;
  uint64_t data;
};

struct open_file {
  uint16_t refcount;
  enum open_file_type type;
  uint64_t offset;
  uint32_t flags;
  uint8_t flock_mode;
  struct open_file *flock_next;
  char path[CELL_PATH_MAX];
  struct vfs_node node;
  union {
    struct {
      uint8_t pipe_id;
      bool pipe_write_end;
    };
    struct {
      uint8_t pty_id;
      bool pty_master;
    };
    struct {
      bool unix_connected;
      uint8_t unix_rx_pipe;
      uint8_t unix_tx_pipe;
      int unix_owner_pid;
      uint32_t unix_owner_uid;
      uint32_t unix_owner_gid;
      int unix_peer_pid;
      uint32_t unix_peer_uid;
      uint32_t unix_peer_gid;
      char unix_path[108];
      uint8_t socket_proto;
      uint32_t udp_remote_ip;
      uint16_t udp_local_port;
      uint16_t udp_remote_port;
      bool udp_connected;
      uint8_t udp_error;
      bool so_reuseaddr;
      bool so_reuseport;
      bool so_keepalive;
      bool so_broadcast;
      bool so_dontroute;
      bool so_linger_enabled;
      bool tcp_nodelay;
      uint32_t so_sndbuf;
      uint32_t so_rcvbuf;
      uint32_t so_linger_seconds;
      uint32_t so_rcvlowat;
      uint64_t so_rcvtimeo_ticks;
      uint64_t so_sndtimeo_ticks;
      uint32_t tcp_keepidle;
      uint32_t tcp_keepintvl;
      uint32_t tcp_keepcnt;
      uint32_t ip_tos;
      uint32_t ip_mtu_discover;
      bool ip_bind_address_no_port;
      uint8_t dgram_rx_head;
      uint8_t dgram_rx_count;
      uint8_t dgram_rx_slot[CELL_DGRAM_RX_QUEUE_CAP];
      uint8_t dgram_rx_ttl[CELL_DGRAM_RX_QUEUE_CAP];
      uint16_t dgram_rx_len[CELL_DGRAM_RX_QUEUE_CAP];
      uint32_t dgram_rx_ip[CELL_DGRAM_RX_QUEUE_CAP];
      uint16_t dgram_rx_port[CELL_DGRAM_RX_QUEUE_CAP];
      uint32_t tcp_remote_ip;
      uint32_t tcp_seq;
      uint32_t tcp_ack;
      uint32_t tcp_remote_window;
      uint32_t tcp_remote_mss;
      uint32_t tcp_srtt_ticks_x8;
      uint32_t tcp_rttvar_ticks_x4;
      uint32_t tcp_rto_ticks;
      uint32_t tcp_user_timeout_ms;
      uint64_t tcp_last_activity_tick;
      uint64_t tcp_keepalive_deadline_tick;
      uint64_t tcp_retransmit_deadline_tick;
      uint64_t tcp_syn_sent_tick;
      uint16_t tcp_local_port;
      uint16_t tcp_remote_port;
      uint16_t tcp_listen_backlog;
      uint32_t tcp_rx_len;
      uint8_t tcp_state;
      uint8_t tcp_error;
      uint8_t tcp_ooo_slot;
      uint8_t tcp_syn_retries;
      uint8_t tcp_keepalive_probes;
      uint8_t tcp_remote_window_scale;
      bool tcp_fin;
      bool tcp_fin_sent;
      bool tcp_fin_pending;
      uint32_t tcp_fin_seq;
      uint32_t ip_ttl;
      bool ip_recvttl;
      uint32_t tcp_tx_bytes;
      uint8_t tcp_tx_count;
      struct tcp_rx_state *tcp_rx;
      struct open_file *tcp_timer_next;
      struct open_file **tcp_timer_link;
      struct open_file *socket_index_next;
      struct tcp_segment *tcp_tx_head;
      struct tcp_segment *tcp_tx_tail;
      struct tcp_segment *tcp_tx_spare;
      struct tcp_ooo_state *tcp_ooo;
    };
    struct epoll_watch epoll_watches[CELL_EPOLL_WATCH_CAP];
    struct {
      uint64_t eventfd_value;
      uint16_t eventfd_waiters;
      bool eventfd_semaphore;
    };
    uint64_t inotify_pa;
  };
};

bool cell_fd_table_reset(size_t capacity);
int cell_find_free_fd(struct domain *domain, int start);
void cell_retain_open_file(struct open_file *file);
void cell_release_open_file(struct open_file *file);
void cell_close_all_fds(struct domain *domain);
bool cell_init_stdio(struct domain *domain);
void cell_copy_fd_table(struct domain *dst, const struct domain *src);
void cell_copy_open_path(struct open_file *file, const char *path);
int64_t cell_fd_lseek(int fd, int64_t off, int whence);
int cell_fd_open_node(const struct vfs_node *node, uint32_t flags, const char *path);
int cell_fd_pipe2(uint64_t pipefd_addr, int flags);
int cell_fd_inotify_init1(int flags);
int cell_fd_inotify_add_watch(int fd, const char *path, uint32_t mask);
int cell_fd_inotify_rm_watch(int fd, int wd);
int cell_fd_dup(int oldfd, int minfd);
int cell_fd_dup3(int oldfd, int newfd, int flags);
struct open_file *cell_fd_file(uint64_t fd);
int cell_fd_get_flags(int fd);
int cell_fd_set_flags(int fd, int flags);
int cell_fd_get_fd_flags(int fd);
int cell_fd_set_fd_flags(int fd, int flags);
int cell_fd_add_seals(int fd, uint32_t seals);
int cell_fd_get_seals(int fd);
int cell_fd_close(int fd);
int cell_fd_is_tty(int fd);
bool cell_fd_stat(int fd, struct vfs_node *out);
int cell_fd_get_dir_node(int fd, struct vfs_node *out, uint64_t *cursor);
bool cell_fd_is_dir(int fd);
bool cell_fd_path(int fd, char *out, size_t cap);
void cell_fd_set_dir_offset(int fd, uint64_t offset);
int cell_fd_flock(int fd, int operation, struct trap_frame *frame);
struct open_file *cell_alloc_open_file(void);
