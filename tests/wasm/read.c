#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc > 2) { return 1; }
  int fd = argc == 1 ? STDIN_FILENO : open(argv[1], O_RDONLY);
  if (fd < 0) { return 2; }
  char bytes[257];
  ssize_t count;
  while ((count = read(fd, bytes, sizeof(bytes))) > 0) {
    if (write(STDOUT_FILENO, bytes, count) != count) { return 3; }
  }
  return count == 0 && !close(fd) ? 42 : 4;
}
