#include <spore.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static const char *emit_escape(const char *p) {
  ++p;
  if (*p == 'n') {
    putchar('\n');
  } else if (*p == 't') {
    putchar('\t');
  } else if (*p == 'r') {
    putchar('\r');
  } else if (*p == '\\') {
    putchar('\\');
  } else if (*p != '\0') {
    putchar(*p);
  } else {
    return p - 1;
  }
  return p;
}

static bool emit_conversion(char spec, int *arg, int argc, char **argv) {
  const char *value = *arg < argc ? argv[(*arg)++] : "";
  switch (spec) {
  case '%':
    putchar('%');
    return false;
  case 'c':
    putchar(value[0] == '\0' ? '\0' : value[0]);
    return true;
  case 's':
    fputs(value, stdout);
    return true;
  case 'd':
  case 'i':
    printf("%d", value[0] == '\0' ? 0 : atoi(value));
    return true;
  case 'u':
    printf("%u", value[0] == '\0' ? 0U : (unsigned)strtoul(value, NULL, 0));
    return true;
  case 'x':
    printf("%x", value[0] == '\0' ? 0U : (unsigned)strtoul(value, NULL, 0));
    return true;
  case 'X':
    printf("%X", value[0] == '\0' ? 0U : (unsigned)strtoul(value, NULL, 0));
    return true;
  default:
    putchar('%');
    putchar(spec);
    return false;
  }
}

static int emit_format(const char *fmt, int *arg, int argc, char **argv) {
  int conversions = 0;
  for (const char *p = fmt; *p != '\0'; ++p) {
    if (*p == '\\') p = emit_escape(p);
    else if (*p == '%' && p[1] != '\0') {
      ++p;
      if (emit_conversion(*p, arg, argc, argv)) { ++conversions; }
    } else putchar(*p);
  }
  return conversions;
}

int main(int argc, char **argv) {
  if (argc < 2) { return usage("printf", "FORMAT [ARGUMENT]..."); }
  const char *fmt = argv[1];
  int arg = 2;
  int conversions = emit_format(fmt, &arg, argc, argv);
  while (conversions > 0 && arg < argc)
    (void)emit_format(fmt, &arg, argc, argv);
  return ferror(stdout) ? EXIT_FAILURE : EXIT_SUCCESS;
}
