long freebsd_call(long, long, long, long, long, long, long);

int freebsd_services(void) {
  long address = freebsd_call(0, 4096, 3, 0x15001002, -1, 0, 477);
  if (address <= 0 || address % (2 << 20)) { return 1; }
  volatile int *page = (volatile int *)address;
  *page = 42;
  if (freebsd_call(address, 4096, 1, 0, 0, 0, 74) || *page != 42) { return 1; }
  if (freebsd_call(address, 4096, 0, 0, 0, 0, 73)) { return 1; }
  if (freebsd_call(-1, 0, 1, 0, 0, 0, 7) != -10) { return 1; }
  return freebsd_call(0, 0, 20, 0, 0, 0, 209) != 0;
}
