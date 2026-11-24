#pragma once
struct open_file;
struct epoll_subscription;

struct epoll_source {
  struct epoll_subscription *first;
};

void cell_epoll_source_activity(struct epoll_source *source, unsigned events);
void cell_epoll_file_activity(struct open_file *file, unsigned events);
void cell_epoll_source_changed(struct epoll_source *source);
void cell_epoll_file_changed(struct open_file *file);
void cell_epoll_sources_changed(struct open_file *file);
void cell_epoll_release_file(struct open_file *file);
