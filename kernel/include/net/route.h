#pragma once
#include <stddef.h>
#include <stdint.h>
struct open_file;
struct route_address {
  uint16_t family, pad;
  uint32_t port, groups;
};
constexpr int ROUTE_FAMILY = 16;
constexpr int ROUTE_PROTOCOL = 0;

int route_bind(struct open_file *file, struct route_address address);
void route_close(struct open_file *file);
int64_t route_send(struct open_file *file, const void *data, size_t len);
