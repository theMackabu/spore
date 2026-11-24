#pragma once

#include "integration.h"

#include <netinet/in.h>

uint32_t integration_ip4(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
void integration_fill_udp_addr(struct sockaddr_in *sa, uint32_t ip, uint16_t port);
int integration_udp_send_target(uint32_t ip, uint16_t port);
int integration_udp_connect_send(uint32_t connect_ip, uint16_t connect_port, uint32_t send_ip, uint16_t send_port,
                                 int null_dest);

int integration_unix_suite(void);
