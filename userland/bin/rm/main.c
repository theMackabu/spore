#include "util.h"

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int rc = SPORE_OK;
  for (int i = 1; i < argc; ++i) {
    if (unlink(argv[i]) != 0) {
      perror("rm");
      rc = SPORE_ERROR;
    }
  }
  return rc;
}
