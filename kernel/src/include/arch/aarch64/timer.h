#pragma once

#include "arch/aarch64/regs.h"

#include <stdint.h>

void timer_init(uint64_t hhdm_offset);
void timer_cpu_init(uint32_t cpu);
void handle_irq(struct trap_frame *frame, uint64_t from_lower_el);
