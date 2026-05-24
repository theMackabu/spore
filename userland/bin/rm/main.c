#include <stdlib.h>

#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int rc = EXIT_SUCCESS;
  for (int i = 1; i < argc; ++i) {
    if (unlink(argv[i]) != 0) {
      perror("rm");
      rc = EXIT_FAILURE;
    }
  }
  return rc;
}
