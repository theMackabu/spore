#pragma once

#include "proc/limits.h"
#include "fs/vfs.h"
#include "fs/notify.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct domain;
struct inotify_state;
struct trap_frame;
struct tcp_fragment;
struct pipe_obj;
struct tcp_segment;
struct socket_datagram;
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

struct epoll_watch;

struct open_file {
  uint32_t refcount;
  enum open_file_type type;
  uint64_t offset;
  uint32_t flags;
  uint8_t flock_mode;
  struct open_file *flock_next;
  struct epoll_watch *epoll_observers;
  char path[CELL_PATH_MAX];
  struct vfs_node node;
  union {
    struct {
      struct pipe_obj *pipe;
      bool name_unlinked, name_unresolved;
      struct fsnotify_name notify_name;
      struct open_file *name_next, **name_link;
    };
    struct {
      uint8_t pty_id;
      bool pty_master;
    };
    struct {
      uint8_t socket_proto;
      uint16_t socket_waiters;
      bool so_reuseaddr;
      bool so_reuseport;
      bool so_keepalive;
      bool so_broadcast;
      bool so_dontroute;
      bool so_linger_enabled;
      uint32_t so_sndbuf;
      uint32_t so_rcvbuf;
      uint32_t so_linger_seconds;
      uint32_t so_rcvlowat;
      uint64_t so_rcvtimeo_ticks;
      uint64_t so_sndtimeo_ticks;
      uint32_t ip_tos;
      uint32_t ip_mtu_discover;
      bool ip_bind_address_no_port;
      uint32_t ip_ttl;
      bool ip_recvttl;
      struct open_file *socket_index_next;
      union {
        struct {
          bool unix_connected;
          struct pipe_obj *unix_rx_pipe;
          struct pipe_obj *unix_tx_pipe;
          int unix_owner_pid;
          uint32_t unix_owner_uid;
          uint32_t unix_owner_gid;
          int unix_peer_pid;
          uint32_t unix_peer_uid;
          uint32_t unix_peer_gid;
          bool unix_bound, unix_pending_owned, unix_reachable;
          uint32_t unix_inflight;
          struct open_file *unix_gc_next, **unix_gc_link, *unix_gc_work;
          uint16_t unix_backlog, unix_pending_count;
          struct open_file *unix_pending_head, *unix_pending_tail, *unix_pending_next;
        };
        struct {
          uint32_t udp_remote_ip;
          uint16_t udp_local_port;
          uint16_t udp_remote_port;
          bool udp_connected;
          uint8_t udp_error;
          uint16_t dgram_rx_bytes;
          uint8_t dgram_rx_count;
          struct socket_datagram *dgram_rx_head, *dgram_rx_tail, *dgram_rx_spare;
        };
        struct {
          bool tcp_nodelay;
          uint32_t tcp_keepidle;
          uint32_t tcp_keepintvl;
          uint32_t tcp_keepcnt;
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
          uint16_t tcp_listen_backlog, tcp_pending_count;
          struct open_file *tcp_half_open, *tcp_ready_head, **tcp_ready_tail;
          struct open_file *tcp_listener, *tcp_pending_next, **tcp_pending_link;
          uint8_t tcp_state;
          uint8_t tcp_error;
          uint8_t tcp_ooo_count;
          uint8_t tcp_syn_retries;
          uint8_t tcp_keepalive_probes;
          uint8_t tcp_remote_window_scale, tcp_local_window_scale;
          bool tcp_fin;
          bool tcp_fin_sent;
          bool tcp_fin_pending;
          uint32_t tcp_fin_seq;
          uint32_t tcp_tx_bytes;
          uint8_t tcp_tx_count;
          struct tcp_rx_state *tcp_rx;
          struct open_file *tcp_timer_next;
          struct open_file **tcp_timer_link;
          struct tcp_segment *tcp_tx_head;
          struct tcp_segment *tcp_tx_tail;
          struct tcp_segment *tcp_tx_spare;
          struct tcp_fragment *tcp_ooo;
        };
      };
    };
    struct {
      struct epoll_watch *epoll_interests, *epoll_ready_head, *epoll_ready_tail;
      uint32_t epoll_ready_count;
      uint64_t epoll_scan;
      unsigned epoll_depth;
    };
    struct {
      uint64_t eventfd_value;
      uint16_t eventfd_waiters;
      bool eventfd_semaphore;
    };
    struct inotify_state *inotify;
  };
};

bool cell_fd_table_reset(size_t capacity);
int cell_find_free_fd(struct domain *domain, int start);
int cell_install_fd(struct domain *domain, struct open_file *file, bool cloexec);
void cell_assign_fd(struct domain *domain, int fd, struct open_file *file, bool cloexec);
int cell_install_fd_pair(struct domain *domain, struct open_file *a, struct open_file *b, bool cloexec, uint64_t out);
void cell_close_exec_fds(struct domain *domain);
int cell_next_open_fd(struct domain *domain, unsigned start);
void cell_retain_open_file(struct open_file *file);
void cell_release_open_file(struct open_file *file);
void cell_close_fd_range(unsigned first, unsigned last, bool cloexec);
void cell_close_all_fds(struct domain *domain);
bool cell_init_stdio(struct domain *domain);
bool cell_copy_fd_table(struct domain *dst, const struct domain *src);
void cell_copy_open_path(struct open_file *file, const char *path);
int64_t cell_fd_lseek(int fd, int64_t off, int whence);
int cell_fd_open_node(const struct vfs_node *node, uint32_t flags, const char *path, const char *resolved);
int cell_fd_pipe2(uint64_t pipefd_addr, int flags);
int cell_fd_dup(int oldfd, int minfd);
int cell_fd_dup3(int oldfd, int newfd, int flags);
struct open_file *cell_fd_file(uint64_t fd);
int cell_fd_get_flags(int fd);
int cell_fd_set_flags(int fd, int flags);
int cell_fd_get_fd_flags(int fd);
int cell_fd_set_fd_flags(int fd, int flags);
int cell_fd_add_seals(int fd, uint32_t seals);
int cell_fd_get_seals(int fd);
int cell_close_domain_fd(struct domain *domain, int fd);
int cell_fd_close(int fd);
int cell_fd_is_tty(int fd);
bool cell_fd_stat(int fd, struct vfs_node *out);
int cell_fd_get_dir_node(int fd, struct vfs_node *out, uint64_t *cursor);
bool cell_fd_is_dir(int fd);
bool cell_fd_path(int fd, char *out, size_t cap);
void cell_fd_set_dir_offset(int fd, uint64_t offset);
int cell_fd_flock(int fd, int operation, struct trap_frame *frame);
struct open_file *cell_alloc_open_file(void);
