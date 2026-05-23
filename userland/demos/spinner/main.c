#include "util.h"

int main(void) {
    volatile unsigned long x = 0;
    for (;;) {
        ++x;
    }
    return SPORE_OK;
}
