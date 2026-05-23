#include "util.h"

#include <stdio.h>
#include <sys/mman.h>

int main(void) {
    void *p = mmap(NULL, 8 * 1024 * 1024, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        puts("memhog: mmap past cap failed cleanly");
        return SPORE_OK;
    }
    puts("memhog: unexpected mmap success");
    return SPORE_ERROR;
}
