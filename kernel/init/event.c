#include "init/event.h"

void app_event(void (*write)(const uint8_t *, size_t), char kind, unsigned value) {
  static bool announced;
  if (!announced) {
    static const uint8_t magic[] = {0, 'S', 'P', 'O', 'R', 'E', '1', 0};
    write(magic, sizeof(magic));
    announced = true;
  }
  uint8_t record[2] = {(uint8_t)kind, (uint8_t)value};
  write(record, sizeof(record));
}
