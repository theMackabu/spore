#pragma once

#include "cell.h"

bool cell_deliver_signal_to_thread(struct thread *thread, int signal);
bool cell_deliver_pending_signals(struct thread *thread);
