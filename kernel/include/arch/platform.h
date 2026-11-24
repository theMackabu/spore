#pragma once
#include <stdint.h>

[[noreturn]] void arch_halt(void);
[[noreturn]] void arch_poweroff(void);
[[noreturn]] void arch_reboot(void);
