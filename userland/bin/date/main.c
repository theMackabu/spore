#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct civil_time {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
  int weekday;
};

static int leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static void civil_from_epoch(long long epoch, struct civil_time *out) {
  static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  long long days = epoch / 86400;
  int rem = (int)(epoch % 86400);
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  out->weekday = (int)((days + 4) % 7);
  if (out->weekday < 0) { out->weekday += 7; }
  out->hour = rem / 3600;
  out->minute = (rem % 3600) / 60;
  out->second = rem % 60;

  int year = 1970;
  while (days >= (leap_year(year) ? 366 : 365)) {
    days -= leap_year(year) ? 366 : 365;
    ++year;
  }
  int month = 1;
  for (;;) {
    int dim = month_days[month - 1] + (month == 2 && leap_year(year) ? 1 : 0);
    if (days < dim) { break; }
    days -= dim;
    ++month;
  }
  out->year = year;
  out->month = month;
  out->day = (int)days + 1;
}

int main(void) {
  static const char *weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
    perror("date");
    return EXIT_FAILURE;
  }

  struct civil_time ct;
  civil_from_epoch((long long)ts.tv_sec - 7 * 60 * 60, &ct);
  printf("%s %s %d %02d:%02d:%02d PDT %d\n", weekdays[ct.weekday], months[ct.month - 1], ct.day, ct.hour, ct.minute,
         ct.second, ct.year);
  return EXIT_SUCCESS;
}
