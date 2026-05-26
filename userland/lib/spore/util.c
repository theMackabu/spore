#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <spore.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

int eprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int rc = vfprintf(stderr, fmt, ap);
  va_end(ap);
  return rc;
}

int usage(const char *tool, const char *usage) {
  eprintf("usage: %s %s\n", tool, usage);
  return EXIT_USAGE;
}

const char *basename(const char *path) {
  const char *name = path;
  for (const char *p = path; *p != '\0'; ++p) {
    if (*p == '/' && p[1] != '\0') { name = p + 1; }
  }
  return name;
}

bool streq(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

static uint16_t be16(uint16_t x) {
  return (uint16_t)((x << 8) | (x >> 8));
}

bool parse_ipv4(const char *s, uint32_t *out) {
  unsigned a;
  unsigned b;
  unsigned c;
  unsigned d;
  char tail;
  if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
    return false;
  }
  *out = a | (b << 8) | (c << 16) | (d << 24);
  return true;
}

void format_ipv4(uint32_t ip, char *out, size_t cap) {
  snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(ip & 0xffu), (unsigned)((ip >> 8) & 0xffu),
           (unsigned)((ip >> 16) & 0xffu), (unsigned)((ip >> 24) & 0xffu));
}

bool net_config_get(struct net_config *out) {
  return out != NULL && syscall(SYS_spore_net_config, 0, out) == 0;
}

bool net_config_set(const struct net_config *cfg) {
  return cfg != NULL && syscall(SYS_spore_net_config, 1, cfg) == 0;
}

static bool hosts_lookup(const char *name, uint32_t *out) {
  FILE *f = fopen("/etc/hosts", "r");
  if (f == NULL) { return false; }
  char line[256];
  while (fgets(line, sizeof(line), f) != NULL) {
    char *hash = strchr(line, '#');
    if (hash != NULL) { *hash = '\0'; }
    char ip_s[64];
    char host_s[128];
    if (sscanf(line, "%63s %127s", ip_s, host_s) != 2) { continue; }
    if (strcmp(host_s, name) == 0 && parse_ipv4(ip_s, out)) {
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

static bool resolv_nameserver(uint32_t *out) {
  FILE *f = fopen("/etc/resolv.conf", "r");
  if (f == NULL) { return false; }
  char line[160];
  while (fgets(line, sizeof(line), f) != NULL) {
    char key[32];
    char value[64];
    if (sscanf(line, "%31s %63s", key, value) == 2 && strcmp(key, "nameserver") == 0 && parse_ipv4(value, out)) {
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

static size_t dns_name(uint8_t *buf, size_t cap, const char *name) {
  size_t out = 0;
  const char *label = name;
  while (*label != '\0') {
    const char *dot = strchr(label, '.');
    size_t len = dot == NULL ? strlen(label) : (size_t)(dot - label);
    if (len == 0 || len > 63 || out + len + 1 >= cap) { return 0; }
    buf[out++] = (uint8_t)len;
    memcpy(buf + out, label, len);
    out += len;
    if (dot == NULL) { break; }
    label = dot + 1;
  }
  if (out >= cap) { return 0; }
  buf[out++] = 0;
  return out;
}

static size_t dns_skip_name(const uint8_t *buf, size_t len, size_t off) {
  while (off < len) {
    uint8_t c = buf[off++];
    if (c == 0) { return off; }
    if ((c & 0xc0u) == 0xc0u) { return off < len ? off + 1 : 0; }
    off += c;
  }
  return 0;
}

static bool dns_query(uint32_t server, const char *name, uint32_t *out) {
  uint8_t query[256] = {0};
  query[0] = 0x12;
  query[1] = 0x34;
  query[2] = 0x01;
  query[5] = 0x01;
  size_t off = 12;
  size_t n = dns_name(query + off, sizeof(query) - off - 4, name);
  if (n == 0) { return false; }
  off += n;
  query[off++] = 0;
  query[off++] = 1;
  query[off++] = 0;
  query[off++] = 1;

  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) { return false; }
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = be16(53);
  sa.sin_addr.s_addr = server;
  ssize_t sent = sendto(fd, query, off, 0, (struct sockaddr *)&sa, sizeof(sa));
  if (sent < 0) {
    close(fd);
    return false;
  }
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  if (poll(&pfd, 1, 1500) <= 0) {
    close(fd);
    return false;
  }
  uint8_t reply[512];
  ssize_t got = recvfrom(fd, reply, sizeof(reply), 0, NULL, NULL);
  close(fd);
  if (got < 12 || reply[0] != 0x12 || reply[1] != 0x34 || (reply[3] & 0x0f) != 0) { return false; }
  size_t len = (size_t)got;
  uint16_t qd = (uint16_t)((reply[4] << 8) | reply[5]);
  uint16_t an = (uint16_t)((reply[6] << 8) | reply[7]);
  off = 12;
  for (uint16_t i = 0; i < qd; ++i) {
    off = dns_skip_name(reply, len, off);
    if (off == 0 || off + 4 > len) { return false; }
    off += 4;
  }
  for (uint16_t i = 0; i < an; ++i) {
    off = dns_skip_name(reply, len, off);
    if (off == 0 || off + 10 > len) { return false; }
    uint16_t type = (uint16_t)((reply[off] << 8) | reply[off + 1]);
    uint16_t klass = (uint16_t)((reply[off + 2] << 8) | reply[off + 3]);
    uint16_t rdlen = (uint16_t)((reply[off + 8] << 8) | reply[off + 9]);
    off += 10;
    if (off + rdlen > len) { return false; }
    if (type == 1 && klass == 1 && rdlen == 4) {
      *out = (uint32_t)reply[off] | ((uint32_t)reply[off + 1] << 8) | ((uint32_t)reply[off + 2] << 16) |
             ((uint32_t)reply[off + 3] << 24);
      return true;
    }
    off += rdlen;
  }
  return false;
}

bool resolve_ipv4(const char *name, uint32_t *out) {
  if (name == NULL || out == NULL) { return false; }
  if (parse_ipv4(name, out)) { return true; }
  if (hosts_lookup(name, out)) { return true; }
  uint32_t server = 0;
  if (!resolv_nameserver(&server)) {
    struct net_config cfg;
    if (net_config_get(&cfg) && cfg.dns_ip != 0) { server = cfg.dns_ip; }
  }
  return server != 0 && dns_query(server, name, out);
}

static int service_port(const char *service, const struct addrinfo *hints, uint16_t *out) {
  if (service == NULL || service[0] == '\0') {
    *out = 0;
    return 0;
  }
  unsigned port = 0;
  for (const char *p = service; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      int socktype = hints == NULL ? 0 : hints->ai_socktype;
      if ((strcmp(service, "http") == 0 || strcmp(service, "www") == 0) && (socktype == 0 || socktype == SOCK_STREAM)) {
        *out = 80;
        return 0;
      }
      if (strcmp(service, "https") == 0 && (socktype == 0 || socktype == SOCK_STREAM)) {
        *out = 443;
        return 0;
      }
      if (strcmp(service, "domain") == 0) {
        *out = 53;
        return 0;
      }
      return EAI_SERVICE;
    }
    port = port * 10u + (unsigned)(*p - '0');
    if (port > 65535) { return EAI_SERVICE; }
  }
  *out = (uint16_t)port;
  return 0;
}

static int addrinfo_family(const struct addrinfo *hints) {
  if (hints == NULL || hints->ai_family == AF_UNSPEC || hints->ai_family == AF_INET) { return AF_INET; }
  return hints->ai_family;
}

static int addrinfo_socktype(const struct addrinfo *hints) {
  if (hints == NULL || hints->ai_socktype == 0) { return SOCK_STREAM; }
  return hints->ai_socktype;
}

static int addrinfo_protocol(int socktype, const struct addrinfo *hints) {
  if (hints != NULL && hints->ai_protocol != 0) { return hints->ai_protocol; }
  if (socktype == SOCK_DGRAM) { return IPPROTO_UDP; }
  if (socktype == SOCK_STREAM) { return IPPROTO_TCP; }
  return 0;
}

int getaddrinfo(const char *restrict node, const char *restrict service, const struct addrinfo *restrict hints,
                struct addrinfo **restrict res) {
  if (res == NULL) { return EAI_FAIL; }
  *res = NULL;

  int family = addrinfo_family(hints);
  if (family != AF_INET) { return EAI_FAMILY; }

  int socktype = addrinfo_socktype(hints);
  if (socktype != SOCK_STREAM && socktype != SOCK_DGRAM) { return EAI_SOCKTYPE; }

  uint16_t port = 0;
  int service_rc = service_port(service, hints, &port);
  if (service_rc != 0) { return service_rc; }

  uint32_t ip = 0;
  if (node == NULL || node[0] == '\0') {
    int flags = hints == NULL ? 0 : hints->ai_flags;
    ip = (flags & AI_PASSIVE) != 0 ? INADDR_ANY : htonl(INADDR_LOOPBACK);
  } else if (!resolve_ipv4(node, &ip)) {
    return EAI_NONAME;
  }

  struct addrinfo *ai = calloc(1, sizeof(*ai));
  struct sockaddr_in *sa = calloc(1, sizeof(*sa));
  if (ai == NULL || sa == NULL) {
    free(ai);
    free(sa);
    return EAI_MEMORY;
  }

  sa->sin_family = AF_INET;
  sa->sin_port = htons(port);
  sa->sin_addr.s_addr = ip;

  ai->ai_family = AF_INET;
  ai->ai_socktype = socktype;
  ai->ai_protocol = addrinfo_protocol(socktype, hints);
  ai->ai_addrlen = sizeof(*sa);
  ai->ai_addr = (struct sockaddr *)sa;
  *res = ai;
  return 0;
}

void freeaddrinfo(struct addrinfo *ai) {
  while (ai != NULL) {
    struct addrinfo *next = ai->ai_next;
    free(ai->ai_addr);
    free(ai->ai_canonname);
    free(ai);
    ai = next;
  }
}

int getnameinfo(const struct sockaddr *restrict sa, socklen_t sl, char *restrict node, socklen_t nodelen,
                char *restrict service, socklen_t servicelen, int flags) {
  if (sa == NULL || sl < sizeof(struct sockaddr_in) || sa->sa_family != AF_INET) { return EAI_FAMILY; }
  const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
  if (node != NULL && nodelen != 0) {
    if ((flags & NI_NUMERICHOST) == 0) {
      char localhost[16];
      format_ipv4(htonl(INADDR_LOOPBACK), localhost, sizeof(localhost));
      if (in->sin_addr.s_addr == htonl(INADDR_LOOPBACK) && strlen("localhost") + 1 <= nodelen) {
        strcpy(node, "localhost");
      } else {
        format_ipv4(in->sin_addr.s_addr, node, nodelen);
      }
    } else {
      format_ipv4(in->sin_addr.s_addr, node, nodelen);
    }
  }
  if (service != NULL && servicelen != 0) { snprintf(service, servicelen, "%u", (unsigned)ntohs(in->sin_port)); }
  return 0;
}

const char *gai_strerror(int errcode) {
  switch (errcode) {
  case 0:
    return "Success";
  case EAI_AGAIN:
    return "Temporary failure in name resolution";
  case EAI_BADFLAGS:
    return "Bad value for ai_flags";
  case EAI_FAIL:
    return "Non-recoverable failure in name resolution";
  case EAI_FAMILY:
    return "Address family not supported";
  case EAI_MEMORY:
    return "Memory allocation failure";
  case EAI_NONAME:
    return "Name does not resolve";
  case EAI_SERVICE:
    return "Service not available";
  case EAI_SOCKTYPE:
    return "Socket type not supported";
  default:
    return "Unknown getaddrinfo error";
  }
}

static void copy_field(char *dst, size_t cap, const char *src) {
  if (cap == 0) { return; }
  size_t n = 0;
  while (n + 1 < cap && src[n] != '\0') {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
}

static bool parse_uint(const char *s, unsigned *out) {
  unsigned v = 0;
  if (*s == '\0') { return false; }
  for (const char *p = s; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') { return false; }
    v = v * 10u + (unsigned)(*p - '0');
  }
  *out = v;
  return true;
}

static bool next_field(char **p, char **field) {
  if (*p == NULL) { return false; }
  *field = *p;
  char *colon = strchr(*p, ':');
  if (colon == NULL) {
    *p = NULL;
  } else {
    *colon = '\0';
    *p = colon + 1;
  }
  return true;
}

static bool parse_passwd_line(char *line, struct user_entry *out) {
  char *nl = strchr(line, '\n');
  if (nl != NULL) { *nl = '\0'; }
  char *p = line;
  char *name = NULL;
  char *password = NULL;
  char *uid_s = NULL;
  char *gid_s = NULL;
  char *gecos = NULL;
  char *home = NULL;
  char *shell = NULL;
  if (!next_field(&p, &name) || !next_field(&p, &password) || !next_field(&p, &uid_s) || !next_field(&p, &gid_s) ||
      !next_field(&p, &gecos) || !next_field(&p, &home) || !next_field(&p, &shell)) {
    return false;
  }
  unsigned uid = 0;
  unsigned gid = 0;
  if (!parse_uint(uid_s, &uid) || !parse_uint(gid_s, &gid)) { return false; }
  copy_field(out->name, sizeof(out->name), name);
  copy_field(out->password, sizeof(out->password), password);
  out->uid = uid;
  out->gid = gid;
  copy_field(out->gecos, sizeof(out->gecos), gecos);
  copy_field(out->home, sizeof(out->home), home);
  copy_field(out->shell, sizeof(out->shell), shell);
  return true;
}

static bool parse_group_line(char *line, struct group_entry *out) {
  char *nl = strchr(line, '\n');
  if (nl != NULL) { *nl = '\0'; }
  char *p = line;
  char *name = NULL;
  char *password = NULL;
  char *gid_s = NULL;
  if (!next_field(&p, &name) || !next_field(&p, &password) || !next_field(&p, &gid_s)) { return false; }
  (void)password;
  unsigned gid = 0;
  if (!parse_uint(gid_s, &gid)) { return false; }
  copy_field(out->name, sizeof(out->name), name);
  out->gid = gid;
  return true;
}

bool user_by_name(const char *name, struct user_entry *out) {
  FILE *f = fopen("/etc/passwd", "r");
  if (f == NULL) { return false; }
  char line[384];
  struct user_entry entry;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_passwd_line(line, &entry) && strcmp(entry.name, name) == 0) {
      if (out != NULL) { *out = entry; }
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

bool user_by_uid(unsigned uid, struct user_entry *out) {
  FILE *f = fopen("/etc/passwd", "r");
  if (f == NULL) { return false; }
  char line[384];
  struct user_entry entry;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_passwd_line(line, &entry) && entry.uid == uid) {
      if (out != NULL) { *out = entry; }
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

bool group_by_name(const char *name, struct group_entry *out) {
  FILE *f = fopen("/etc/group", "r");
  if (f == NULL) { return false; }
  char line[256];
  struct group_entry entry;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_group_line(line, &entry) && strcmp(entry.name, name) == 0) {
      if (out != NULL) { *out = entry; }
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

bool group_by_gid(unsigned gid, struct group_entry *out) {
  FILE *f = fopen("/etc/group", "r");
  if (f == NULL) { return false; }
  char line[256];
  struct group_entry entry;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_group_line(line, &entry) && entry.gid == gid) {
      if (out != NULL) { *out = entry; }
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

unsigned next_user_id(void) {
  FILE *f = fopen("/etc/passwd", "r");
  if (f == NULL) { return 1000; }
  char line[384];
  struct user_entry entry;
  unsigned max = 999;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (parse_passwd_line(line, &entry) && entry.uid > max) { max = entry.uid; }
  }
  fclose(f);
  return max + 1;
}

static uint64_t fnv1a_mix(uint64_t h, const char *s) {
  while (*s != '\0') {
    h ^= (unsigned char)*s++;
    h *= 1099511628211ull;
  }
  return h;
}

static void hash_password(const char *name, const char *password, char *out, size_t cap) {
  uint64_t h = 1469598103934665603ull;
  h = fnv1a_mix(h, "spore-shadow-v1:");
  h = fnv1a_mix(h, name);
  h = fnv1a_mix(h, ":");
  h = fnv1a_mix(h, password);
  h ^= h >> 32;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  snprintf(out, cap, "$spore$%016llx", (unsigned long long)h);
}

static bool shadow_field(const char *name, char *out, size_t cap) {
  FILE *f = fopen("/etc/shadow", "r");
  if (f == NULL) { return false; }
  char line[256];
  size_t name_len = strlen(name);
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, name, name_len) == 0 && line[name_len] == ':') {
      char *p = line + name_len + 1;
      char *end = strchr(p, ':');
      if (end == NULL) { end = strchr(p, '\n'); }
      if (end != NULL) { *end = '\0'; }
      copy_field(out, cap, p);
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

bool password_matches(const char *name, const char *password) {
  char stored[96];
  if (!shadow_field(name, stored, sizeof(stored))) { return false; }
  if (stored[0] == '\0') { return true; }
  if (stored[0] == '!') { return false; }
  char hashed[96];
  hash_password(name, password, hashed, sizeof(hashed));
  return strcmp(stored, hashed) == 0;
}

static bool rewrite_shadow(const char *name, const char *field, bool remove) {
  FILE *in = fopen("/etc/shadow", "r");
  FILE *out = fopen("/etc/shadow.tmp", "w");
  if (out == NULL) {
    if (in != NULL) { fclose(in); }
    return false;
  }
  bool found = false;
  size_t name_len = strlen(name);
  if (in != NULL) {
    char line[256];
    while (fgets(line, sizeof(line), in) != NULL) {
      if (strncmp(line, name, name_len) == 0 && line[name_len] == ':') {
        found = true;
        if (!remove) { fprintf(out, "%s:%s:0:0:99999:7:::\n", name, field); }
      } else {
        fputs(line, out);
      }
    }
    fclose(in);
  }
  if (!found && !remove) { fprintf(out, "%s:%s:0:0:99999:7:::\n", name, field); }
  fclose(out);
  return rename("/etc/shadow.tmp", "/etc/shadow") == 0;
}

bool set_shadow_password(const char *name, const char *password) {
  char hashed[96];
  hash_password(name, password, hashed, sizeof(hashed));
  return rewrite_shadow(name, hashed, false);
}

bool add_shadow_user(const char *name) {
  return rewrite_shadow(name, "", false);
}

bool remove_shadow_user(const char *name) {
  return rewrite_shadow(name, "", true);
}

static bool group_contains_user(const char *group_name, const char *user_name) {
  struct user_entry user;
  if (!user_by_name(user_name, &user)) { return false; }

  FILE *f = fopen("/etc/group", "r");
  if (f == NULL) { return false; }
  char line[256];
  while (fgets(line, sizeof(line), f) != NULL) {
    char *nl = strchr(line, '\n');
    if (nl != NULL) { *nl = '\0'; }
    char *p = line;
    char *name = NULL;
    char *password = NULL;
    char *gid_s = NULL;
    char *members = NULL;
    if (!next_field(&p, &name) || !next_field(&p, &password) || !next_field(&p, &gid_s)) { continue; }
    (void)password;
    if (!next_field(&p, &members)) { members = ""; }
    if (strcmp(name, group_name) != 0) { continue; }

    unsigned gid = 0;
    if (parse_uint(gid_s, &gid) && user.gid == gid) {
      fclose(f);
      return true;
    }

    for (char *m = members; m != NULL;) {
      char *next = strchr(m, ',');
      if (next != NULL) { *next++ = '\0'; }
      if (strcmp(m, user_name) == 0) {
        fclose(f);
        return true;
      }
      m = next;
    }
  }
  fclose(f);
  return false;
}

bool sudo_user_allowed(const char *name, bool *nopasswd) {
  FILE *f = fopen("/etc/sudoers", "r");
  if (f == NULL) { return false; }
  char line[256];
  bool allowed = false;
  bool no_password = false;
  while (fgets(line, sizeof(line), f) != NULL) {
    char *p = line;
    while (*p == ' ' || *p == '\t') {
      ++p;
    }
    if (*p == '\0' || *p == '\n' || *p == '#') { continue; }
    char *subject = p;
    while (*p != '\0' && *p != '\n' && *p != ' ' && *p != '\t') {
      ++p;
    }
    char saved = *p;
    *p = '\0';
    bool subject_matches = subject[0] == '%' ? group_contains_user(subject + 1, name) : strcmp(subject, name) == 0;
    *p = saved;
    if (subject_matches) {
      allowed = true;
      if (strstr(p, "NOPASSWD") != NULL) { no_password = true; }
    }
  }
  fclose(f);
  if (nopasswd != NULL) { *nopasswd = no_password; }
  return allowed;
}
