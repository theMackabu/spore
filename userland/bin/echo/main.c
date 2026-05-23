#include "util.h"

#include <stdio.h>

int main(int argc, char **argv) {
    (void)argv;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            putchar(' ');
        }
        fputs(argv[i], stdout);
    }
    putchar('\n');
    return SPORE_OK;
}
