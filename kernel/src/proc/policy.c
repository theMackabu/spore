#include "cell.h"

#include "kprintf.h"
#include "kstr.h"
#include "mem.h"
#include "proc/domain.h"

enum {
  IPPROTO_TCP = 6,
  IPPROTO_UDP = 17,
};

static bool parse_dec(const char **cursor, uint32_t max, uint32_t *out) {
  uint32_t value = 0;
  const char *p = *cursor;
  if (*p < '0' || *p > '9') { return false; }
  while (*p >= '0' && *p <= '9') {
    value = value * 10u + (uint32_t)(*p - '0');
    if (value > max) { return false; }
    ++p;
  }
  *cursor = p;
  *out = value;
  return true;
}

static bool parse_ipv4_port(const char *s, uint32_t *ip, uint8_t *prefix, uint16_t *port) {
  uint32_t a;
  uint32_t b;
  uint32_t c;
  uint32_t d;
  uint32_t prefix_value = 32;
  uint32_t p;
  if (!parse_dec(&s, 255, &a) || *s++ != '.' || !parse_dec(&s, 255, &b) || *s++ != '.' || !parse_dec(&s, 255, &c) ||
      *s++ != '.' || !parse_dec(&s, 255, &d)) {
    return false;
  }
  if (*s == '/') {
    ++s;
    if (!parse_dec(&s, 32, &prefix_value)) { return false; }
  }
  if (*s++ != ':' || !parse_dec(&s, 65535, &p) || *s != '\0') { return false; }
  *ip = a | (b << 8) | (c << 16) | (d << 24);
  *prefix = (uint8_t)prefix_value;
  *port = (uint16_t)p;
  return true;
}

static uint32_t egress_mask(uint8_t prefix) {
  if (prefix == 0) { return 0; }
  return 0xffffffffu >> (32u - prefix);
}

static bool egress_match(const struct capability_set *caps, uint8_t proto, uint32_t ip, uint16_t port) {
  if ((caps->flags & CAP_EGRESS_ENFORCE) == 0) { return true; }
  uint32_t mask = egress_mask(caps->egress_prefix);
  return caps->egress_proto == proto && caps->egress_port == port && ((caps->egress_ip ^ ip) & mask) == 0;
}

static bool caps_subset(const struct capability_set *requested, const struct capability_set *parent) {
  for (size_t i = 0; i < sizeof(requested->syscall_allow) / sizeof(requested->syscall_allow[0]); ++i) {
    if ((requested->syscall_allow[i] & ~parent->syscall_allow[i]) != 0) { return false; }
  }
  if ((requested->flags & CAP_EGRESS_ENFORCE) != 0) {
    if ((parent->flags & CAP_EGRESS_ENFORCE) == 0) { return true; }
    uint32_t parent_mask = egress_mask(parent->egress_prefix);
    uint32_t requested_mask = egress_mask(requested->egress_prefix);
    bool cidr_subset =
      parent->egress_proto == requested->egress_proto && parent->egress_port == requested->egress_port &&
      parent->egress_prefix <= requested->egress_prefix &&
      ((parent->egress_ip ^ requested->egress_ip) & parent_mask) == 0 &&
      ((requested->egress_ip & requested_mask) == requested->egress_ip || requested->egress_prefix == 32);
    if (!cidr_subset) { return false; }
  }
  if ((requested->flags & CAP_EGRESS_ENFORCE) == 0 && (parent->flags & CAP_EGRESS_ENFORCE) != 0) { return false; }
  if (parent->memory_page_cap != 0 &&
      (requested->memory_page_cap == 0 || requested->memory_page_cap > parent->memory_page_cap)) {
    return false;
  }
  if (parent->fs_rule_count != 0) {
    for (uint8_t i = 0; i < requested->fs_rule_count; ++i) {
      bool covered = false;
      for (uint8_t j = 0; j < parent->fs_rule_count; ++j) {
        size_t parent_len = kstrlen(parent->fs_rules[j].path);
        bool path_covered = str_eq(parent->fs_rules[j].path, "/") ||
                            (starts_with(requested->fs_rules[i].path, parent->fs_rules[j].path) &&
                             (requested->fs_rules[i].path[parent_len] == '\0' ||
                              requested->fs_rules[i].path[parent_len] == '/'));
        bool rights_covered = (requested->fs_rules[i].rights & ~parent->fs_rules[j].rights) == 0;
        if (path_covered && rights_covered) {
          covered = true;
          break;
        }
      }
      if (!covered) { return false; }
    }
  }
  return true;
}

static void cap_allow(struct capability_set *caps, uint64_t nr) {
  if (nr < 512) { caps->syscall_allow[nr / 64] |= 1ull << (nr % 64); }
}

static void cap_allow_common(struct capability_set *caps) {
  static const uint16_t common[] = {
    17,  19,  23,  24,  25,  29,  57,  59,  63,  64,  65,  66,  72,  73,  80,  93,  94,  96,  98,  99,
    101, 103, 48,
    113, 115, 123, 124, 134, 135, 144, 146, 160, 161, 172, 173, 174, 175, 176, 177, 178, 179, 198,
    200, 203, 204, 206, 207, 208, 209, 211, 212, 214, 215, 216, 220, 221, 222, 226, 233, 260, 261,
    278, 439,
  };
  for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); ++i) {
    cap_allow(caps, common[i]);
  }
}

static void cap_allow_files(struct capability_set *caps) {
  static const uint16_t files[] = {34, 35, 38, 46, 49, 50, 52, 53, 54, 55, 56, 61, 62, 78, 79, 82, 276};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    cap_allow(caps, files[i]);
  }
}

static bool cap_add_fs_rule(struct capability_set *caps, const char *path, uint8_t rights) {
  if (caps == NULL || path == NULL || path[0] != '/' || caps->fs_rule_count >= CELL_FS_RULE_CAP) { return false; }
  struct fs_rule *rule = &caps->fs_rules[caps->fs_rule_count++];
  copy_cstr(rule->path, sizeof(rule->path), path);
  rule->rights = rights;
  return true;
}

static bool path_matches_rule(const char *path, const struct fs_rule *rule) {
  if (str_eq(rule->path, "/")) { return true; }
  size_t len = kstrlen(rule->path);
  return starts_with(path, rule->path) && (path[len] == '\0' || path[len] == '/');
}

bool cell_fs_path_allowed(const char *path, uint8_t rights) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || (domain->caps.flags & CAP_ENFORCE) == 0 || domain->caps.fs_rule_count == 0) { return true; }
  for (uint8_t i = 0; i < domain->caps.fs_rule_count; ++i) {
    const struct fs_rule *rule = &domain->caps.fs_rules[i];
    if (path_matches_rule(path, rule) && (rights == 0 || (rule->rights & rights) == rights)) { return true; }
  }
  return false;
}

bool cell_syscall_allowed(uint64_t nr) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || (domain->caps.flags & CAP_ENFORCE) == 0) { return true; }
  if (nr >= 512) { return false; }
  return (domain->caps.syscall_allow[nr / 64] & (1ull << (nr % 64))) != 0;
}

bool cell_egress_allowed(uint8_t proto, uint32_t ip, uint16_t port) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || (domain->caps.flags & CAP_EGRESS_ENFORCE) == 0) { return true; }
  bool allowed = egress_match(&domain->caps, proto, ip, port);
  if (!allowed) {
    kprintf("[spore] egress denied domain=%d proto=%u dst=%x:%u\n", domain->id, (unsigned)proto, (unsigned)ip,
            (unsigned)port);
  }
  return allowed;
}

bool cell_mmap_allowed(uint64_t pages) {
  struct domain *domain = cell_current_domain_internal();
  return domain == NULL || domain->caps.memory_page_cap == 0 || pages <= domain->caps.memory_page_cap;
}

int cell_apply_policy(const char *manifest) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -3; }
  if (str_eq(manifest, "bad-manifest")) { return -1; }

  struct capability_set caps = {0};
  cap_allow_common(&caps);
  caps.flags = CAP_ENFORCE;
  domain->fs_root[0] = '/';
  domain->fs_root[1] = '\0';

  if (str_eq(manifest, "compute-only")) {
    domain->budget.max_ticks = 20;
    domain->budget.remaining_ticks = 20;
  } else if (starts_with(manifest, "fs:/tmp")) {
    cap_allow_files(&caps);
    if (!cap_add_fs_rule(&caps, "/tmp", CELL_FS_READ | CELL_FS_WRITE | CELL_FS_EXEC) ||
        !cap_add_fs_rule(&caps, "/lib", CELL_FS_READ | CELL_FS_EXEC) ||
        !cap_add_fs_rule(&caps, "/usr/local/lib", CELL_FS_READ | CELL_FS_EXEC)) {
      return -2;
    }
    if (starts_with(manifest, "fs:/tmp;exec:")) {
      const char *exec_path = manifest + sizeof("fs:/tmp;exec:") - 1;
      if (!cap_add_fs_rule(&caps, exec_path, CELL_FS_READ | CELL_FS_EXEC)) { return -2; }
    } else if (!str_eq(manifest, "fs:/tmp")) {
      return -2;
    }
    domain->fs_root[0] = '/';
    domain->fs_root[1] = 't';
    domain->fs_root[2] = 'm';
    domain->fs_root[3] = 'p';
    domain->fs_root[4] = '\0';
  } else if (str_eq(manifest, "mem:1")) {
    caps.memory_page_cap = 1;
  } else if (str_eq(manifest, "net:none")) {
    cap_allow_files(&caps);
    caps.flags |= CAP_EGRESS_ENFORCE;
    caps.egress_prefix = 32;
  } else if (str_eq(manifest, "net:dns")) {
    cap_allow_files(&caps);
    caps.flags |= CAP_EGRESS_ENFORCE;
    caps.egress_proto = IPPROTO_UDP;
    caps.egress_prefix = 0;
    caps.egress_port = 53;
  } else if (starts_with(manifest, "net:udp:")) {
    uint32_t ip = 0;
    uint8_t prefix = 32;
    uint16_t port = 0;
    if (!parse_ipv4_port(manifest + 8, &ip, &prefix, &port)) { return -2; }
    cap_allow_files(&caps);
    caps.flags |= CAP_EGRESS_ENFORCE;
    caps.egress_proto = IPPROTO_UDP;
    caps.egress_prefix = prefix;
    caps.egress_ip = ip & egress_mask(prefix);
    caps.egress_port = port;
  } else if (starts_with(manifest, "net:tcp:")) {
    uint32_t ip = 0;
    uint8_t prefix = 32;
    uint16_t port = 0;
    if (!parse_ipv4_port(manifest + 8, &ip, &prefix, &port)) { return -2; }
    cap_allow_files(&caps);
    caps.flags |= CAP_EGRESS_ENFORCE;
    caps.egress_proto = IPPROTO_TCP;
    caps.egress_prefix = prefix;
    caps.egress_ip = ip & egress_mask(prefix);
    caps.egress_port = port;
  } else {
    return -2;
  }
  if ((domain->caps.flags & CAP_ENFORCE) != 0 && !caps_subset(&caps, &domain->caps)) { return -1; }
  domain->caps = caps;
  kprintf("[spore] policy applied: %s\n", manifest);
  return 0;
}
