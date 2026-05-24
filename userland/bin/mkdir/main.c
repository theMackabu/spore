#include "util.h"

#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
  int rc = SPORE_OK;
  for (int i = 1; i < argc; ++i) {
    if (mkdir(argv[i], 0777) != 0) {
      perror("mkdir");
      rc = SPORE_ERROR;
    }
  }
  return rc;
}
