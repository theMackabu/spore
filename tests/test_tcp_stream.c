#include "../kernel/net/socket/tcp/internal.h"
#include "mm/pmm.h"
#include "net/net.h"
#include "proc/accounting.h"
#include "proc/epoll.h"
#include <assert.h>
#include <string.h>

static struct open_file sender;
static uint64_t ticks = 7;
static uint32_t wire_seq;
static uint8_t wire[1460];
static size_t wire_len;
static bool link_up = true;
static bool auto_ack;

uint64_t cell_uptime_ticks(void) { return ticks; }
uint32_t socket_effective_buffer(const struct open_file *, bool receive) { return receive ? 90000 : UINT16_MAX; }
uint8_t socket_effective_ttl(const struct open_file *) { return 64; }
static size_t syn_options_size;
static uint16_t syn_window;
bool net_tcp_send_segment_options_ttl_tos(uint16_t, uint32_t, uint16_t, uint8_t, uint8_t, uint32_t, uint32_t,
                                          uint16_t window, uint8_t, const void *, size_t, const void *, size_t len) {
  syn_window = window;
  syn_options_size = len;
  return true;
}

static void test_window_negotiation(void) {
  struct open_file peer = {0};
  tcp_apply_syn_options(&peer, 4096, NULL, 0);
  assert(peer.tcp_remote_mss == 536 && tcp_window(&peer) == UINT16_MAX);
  assert(tcp_send_syn(&peer, 10, true) && syn_window == UINT16_MAX && syn_options_size == 4);
  uint8_t options[] = {2, 4, 1, 128, 3, 3, 0};
  tcp_apply_syn_options(&peer, 8192, options, sizeof(options));
  assert(peer.tcp_remote_mss == 384 && tcp_window(&peer) == 22500);
  assert(tcp_send_syn(&peer, 20, true) && syn_window == UINT16_MAX && syn_options_size == 8);
  options[6] = 3;
  tcp_apply_syn_options(&peer, 8192, options, sizeof(options));
  assert(peer.tcp_remote_window == 8192 && peer.tcp_remote_window_scale == 3);
}

bool net_tcp_send_segment_ttl_tos(uint16_t, uint32_t, uint16_t, uint8_t, uint8_t,
                                  uint32_t seq, uint32_t, uint16_t, uint8_t, const void *payload,
                                  size_t len) {
  if (!link_up) { return false; }
  assert(len <= sizeof(wire));
  wire_seq = seq;
  wire_len = len;
  memcpy(wire, payload, len);
  if (auto_ack) { tcp_tx_ack(&sender, seq + (uint32_t)len); }
  return true;
}

static int fill(void *source, uint8_t *dst, size_t len) {
  const char **text = source;
  memcpy(dst, *text, len);
  *text += len;
  return 0;
}

static int64_t send_text(const char *s) { return tcp_write_stream(&sender, strlen(s), fill, &s); }

void tcp_ack_current_state(struct open_file *) {}
void cell_epoll_file_changed(struct open_file *) {}

static int collect(void *destination, const uint8_t *src, size_t len) {
  char **dst = destination;
  memcpy(*dst, src, len);
  *dst += len;
  return 0;
}

static void test_reordered_stream(void) {
  struct open_file stream = {.type = OPEN_SOCKET, .socket_proto = IPPROTO_TCP, .tcp_ack = UINT32_MAX - 3};
  tcp_receive_segment(&stream, 2, (const uint8_t *)"gh", 2, true);
  tcp_receive_segment(&stream, 0, (const uint8_t *)"efg", 3, false);
  tcp_receive_segment(&stream, 2, (const uint8_t *)"gh", 2, false);
  tcp_receive_segment(&stream, UINT32_MAX - 3, (const uint8_t *)"abcde", 5, false);
  char result[8], *dst = result;
  assert(tcp_read_stream(&stream, sizeof(result), 0, collect, &dst) == 8);
  assert(memcmp(result, "abcdefgh", 8) == 0);
  assert(tcp_read_stream(&stream, sizeof(result), 0, collect, &dst) == 0);
  tcp_free_rx(&stream);
  struct open_file other = {.type = OPEN_SOCKET, .socket_proto = IPPROTO_TCP, .tcp_ack = 64};
  tcp_receive_segment(&other, 80, (const uint8_t *)"later", 5, false);
  tcp_free_ooo(&other);
}

int main(void) {
  test_window_negotiation();
  alignas(PAGE_SIZE) static uint8_t backing[32 * PAGE_SIZE];
  struct spore_memmap_entry memory = {.base = 0x100000, .length = sizeof(backing), .type = SPORE_MEMMAP_USABLE};
  pmm_init((uintptr_t)backing - memory.base, &memory, 1);
  uint64_t free_pages = pmm_free_pages();
  test_reordered_stream();
  assert(pmm_free_pages() == free_pages);
  sender = (struct open_file){.type = OPEN_SOCKET, .socket_proto = IPPROTO_TCP, .tcp_state = TCP_ESTABLISHED,
                              .tcp_seq = UINT32_MAX - 2, .tcp_remote_window = 6};
  tcp_init_rto(&sender);
  assert(send_text("abcdefg") == 6);
  assert(wire_seq == UINT32_MAX - 2 && wire_len == 6 && memcmp(wire, "abcdef", 6) == 0);
  assert(send_text("g") == -EAGAIN);
  tcp_tx_ack(&sender, 0);
  assert(send_text("ghi") == 3 && wire_seq == 3);
  uint64_t due = ticks + tcp_current_rto(&sender);
  assert(tcp_tx_retransmit(&sender, due - 1) == 0 && wire_seq == 3);
  ticks = due;
  assert(tcp_tx_retransmit(&sender, ticks) == 0);
  assert(wire_seq == 0 && wire_len == 3 && memcmp(wire, "def", 3) == 0);
  tcp_tx_ack(&sender, 6);
  link_up = false;
  assert(send_text("retry") == -EIO);
  link_up = true;
  auto_ack = true;
  assert(send_text("retry!") == 6 && wire_seq == 6);
  assert(send_text("more") == 4 && wire_seq == 12);
  auto_ack = false;
  sender.tcp_user_timeout_ms = 1000;
  assert(send_text("expire") == 6);
  assert(tcp_tx_retransmit(&sender, ticks + 100) == 110);
  tcp_tx_clear(&sender);
  assert(pmm_free_pages() == free_pages);
}
