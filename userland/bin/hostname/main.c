#include <spore.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_uname
#define SYS_uname 160
#endif
#ifndef SYS_sethostname
#define SYS_sethostname 161
#endif

struct spore_utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

static void print_help(void) {
  puts("usage: hostname [OPTION]... [NAME]");
  puts("Show or set the system host name.");
  puts("");
  puts("  -s, --short           short host name");
  puts("  -f, --fqdn            fully qualified domain name");
  puts("  -d, --domain          DNS domain name");
  puts("  -i, --ip-address      address for this host name");
  puts("  -I, --all-ip-addresses all local addresses");
  puts("  -F, --file FILE       set host name from FILE");
  puts("      --help            display this help and exit");
}

static bool current_hostname(char *out, size_t cap) {
  struct spore_utsname u;
  if (syscall(SYS_uname, &u) != 0) { return false; }
  snprintf(out, cap, "%s", u.nodename);
  return true;
}

static void short_hostname(const char *name, char *out, size_t cap) {
  size_t n = 0;
  while (name[n] != '\0' && name[n] != '.' && n + 1 < cap) {
    out[n] = name[n];
    ++n;
  }
  out[n] = '\0';
}

static const char *domain_part(const char *name) {
  const char *dot = strchr(name, '.');
  return dot == NULL || dot[1] == '\0' ? "" : dot + 1;
}

static bool read_first_line(const char *path, char *out, size_t cap) {
  FILE *f = fopen(path, "r");
  if (f == NULL) { return false; }
  bool ok = fgets(out, (int)cap, f) != NULL;
  fclose(f);
  if (!ok) { return false; }
  out[strcspn(out, "\r\n")] = '\0';
  return out[0] != '\0';
}

static int persist_hostname(const char *name) {
  FILE *f = fopen("/etc/hostname", "w");
  if (f == NULL) { return -1; }
  fprintf(f, "%s\n", name);
  fclose(f);
  return 0;
}

static int set_name(const char *name) {
  size_t len = strlen(name);
  if (len == 0 || len > 64) {
    eprintf("hostname: invalid host name\n");
    return EXIT_FAILURE;
  }
  if (syscall(SYS_sethostname, name, len) != 0) {
    perror("hostname");
    return EXIT_FAILURE;
  }
  if (persist_hostname(name) != 0) { perror("/etc/hostname"); }
  return EXIT_SUCCESS;
}

static void print_ip_for_name(const char *name, bool all) {
  struct net_config cfg;
  uint32_t ip = 0;
  if (resolve_ipv4(name, &ip)) {
    char ip_s[32];
    format_ipv4(ip, ip_s, sizeof(ip_s));
    printf("%s", ip_s);
    if (!all) {
      putchar('\n');
      return;
    }
  }
  if (net_config_get(&cfg) && cfg.local_ip != 0 && cfg.local_ip != ip) {
    char ip_s[32];
    if (ip != 0) { putchar(' '); }
    format_ipv4(cfg.local_ip, ip_s, sizeof(ip_s));
    printf("%s", ip_s);
  }
  putchar('\n');
}

int main(int argc, char **argv) {
  enum {
    MODE_NAME,
    MODE_SHORT,
    MODE_FQDN,
    MODE_DOMAIN,
    MODE_IP,
    MODE_ALL_IP,
  } mode = MODE_NAME;

  const char *set_arg = NULL;
  const char *file_arg = NULL;

  for (int i = 1; i < argc; ++i) {
    if (streq(argv[i], "--help") || streq(argv[i], "-h")) {
      print_help();
      return EXIT_SUCCESS;
    }
    if (streq(argv[i], "-s") || streq(argv[i], "--short")) {
      mode = MODE_SHORT;
    } else if (streq(argv[i], "-f") || streq(argv[i], "--fqdn") || streq(argv[i], "--long")) {
      mode = MODE_FQDN;
    } else if (streq(argv[i], "-d") || streq(argv[i], "--domain")) {
      mode = MODE_DOMAIN;
    } else if (streq(argv[i], "-i") || streq(argv[i], "--ip-address")) {
      mode = MODE_IP;
    } else if (streq(argv[i], "-I") || streq(argv[i], "--all-ip-addresses")) {
      mode = MODE_ALL_IP;
    } else if ((streq(argv[i], "-F") || streq(argv[i], "--file")) && i + 1 < argc) {
      file_arg = argv[++i];
    } else if (argv[i][0] == '-') {
      eprintf("hostname: unknown option: %s\n", argv[i]);
      return usage("hostname", "[OPTION]... [NAME]");
    } else if (set_arg == NULL) {
      set_arg = argv[i];
    } else {
      eprintf("hostname: extra operand: %s\n", argv[i]);
      return EXIT_USAGE;
    }
  }

  if (file_arg != NULL) {
    char name[65];
    if (!read_first_line(file_arg, name, sizeof(name))) {
      perror(file_arg);
      return EXIT_FAILURE;
    }
    return set_name(name);
  }
  if (set_arg != NULL) { return set_name(set_arg); }

  char name[65];
  if (!current_hostname(name, sizeof(name))) {
    perror("hostname");
    return EXIT_FAILURE;
  }

  if (mode == MODE_SHORT) {
    char short_name[65];
    short_hostname(name, short_name, sizeof(short_name));
    puts(short_name);
  } else if (mode == MODE_DOMAIN) {
    puts(domain_part(name));
  } else if (mode == MODE_IP || mode == MODE_ALL_IP) {
    print_ip_for_name(name, mode == MODE_ALL_IP);
  } else {
    puts(name);
  }
  return EXIT_SUCCESS;
}
