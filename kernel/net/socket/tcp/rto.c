#include "internal.h"

enum {
  TCP_INITIAL_RTO_TICKS = 100,
  TCP_MIN_RTO_TICKS = 10,
  TCP_MAX_RTO_TICKS = 6000,
  TCP_MAX_RTO_SHIFT = 6,
};

static uint32_t tcp_clamp_rto(uint64_t ticks) {
  if (ticks < TCP_MIN_RTO_TICKS) { return TCP_MIN_RTO_TICKS; }
  if (ticks > TCP_MAX_RTO_TICKS) { return TCP_MAX_RTO_TICKS; }
  return (uint32_t)ticks;
}

void tcp_init_rto(struct open_file *file) {
  if (file == NULL) { return; }
  file->tcp_srtt_ticks_x8 = 0;
  file->tcp_rttvar_ticks_x4 = 0;
  file->tcp_rto_ticks = TCP_INITIAL_RTO_TICKS;
  file->tcp_syn_sent_tick = 0;
}

uint32_t tcp_current_rto(const struct open_file *file) {
  if (file == NULL || file->tcp_rto_ticks == 0) { return TCP_INITIAL_RTO_TICKS; }
  return tcp_clamp_rto(file->tcp_rto_ticks);
}

uint64_t tcp_backoff_rto(const struct open_file *file, uint8_t retries) {
  uint32_t rto = tcp_current_rto(file);
  uint8_t shift = retries < TCP_MAX_RTO_SHIFT ? retries : TCP_MAX_RTO_SHIFT;
  if (rto > (TCP_MAX_RTO_TICKS >> shift)) { return TCP_MAX_RTO_TICKS; }
  return (uint64_t)rto << shift;
}

void tcp_note_rtt_sample(struct open_file *file, uint64_t sent_tick, uint64_t now_ticks) {
  if (file == NULL || sent_tick == 0) { return; }
  uint64_t raw_sample = now_ticks > sent_tick ? now_ticks - sent_tick : 1;
  uint32_t sample = raw_sample > TCP_MAX_RTO_TICKS ? TCP_MAX_RTO_TICKS : (uint32_t)raw_sample;
  if (sample == 0) { sample = 1; }

  if (file->tcp_srtt_ticks_x8 == 0) {
    file->tcp_srtt_ticks_x8 = sample * 8u;
    file->tcp_rttvar_ticks_x4 = sample * 2u;
  } else {
    uint32_t srtt = file->tcp_srtt_ticks_x8 / 8u;
    uint32_t err = sample > srtt ? sample - srtt : srtt - sample;
    file->tcp_rttvar_ticks_x4 = (file->tcp_rttvar_ticks_x4 * 3u) / 4u + err;
    file->tcp_srtt_ticks_x8 = (file->tcp_srtt_ticks_x8 * 7u) / 8u + sample;
  }

  uint32_t srtt = file->tcp_srtt_ticks_x8 / 8u;
  uint32_t variance = file->tcp_rttvar_ticks_x4 == 0 ? 1u : file->tcp_rttvar_ticks_x4;
  file->tcp_rto_ticks = tcp_clamp_rto((uint64_t)srtt + variance);
}
