#include "proc/policy.h"
#include "lib/kprintf.h"
#include "fs/vfs.h"
#include "lib/kstr.h"
#include "lib/mem.h"
#include "mm/pool.h"
#include "proc/domain.h"
#include "proc/memory.h"

struct egress_rule {
  uint32_t address;
  uint32_t mask;
  uint16_t port;
  uint8_t protocol;
};

struct capability_set {
  uint64_t syscall_allow[8];
  uint64_t memory_page_cap;
  struct egress_rule egress;
  bool restrict_egress;
  bool restrict_fs;
  unsigned refs;
  char executable[CELL_PATH_MAX];
};

static struct object_pool policies;

bool cell_policy_init(size_t domains) {
  return pool_init(&policies, sizeof(struct capability_set), domains + 1);
}

struct capability_set *cell_policy_retain(struct capability_set *caps) {
  if (caps != NULL) { ++caps->refs; }
  return caps;
}

void cell_policy_release(struct capability_set *caps) {
  if (caps != NULL && --caps->refs == 0) { pool_free(&policies, caps); }
}

enum policy_profile { POLICY_COMPUTE, POLICY_FILES, POLICY_MEMORY, POLICY_NETWORK };
struct policy_request {
  enum policy_profile profile;
  struct egress_rule network;
  const char *executable;
};
extern bool spore_parse_manifest(const char *text, struct policy_request *out);

static bool under_path(const char *path, const char *base) {
  if (str_eq(base, "/")) { return true; }
  size_t len = kstrlen(base);
  return starts_with(path, base) && (path[len] == '\0' || path[len] == '/');
}

static bool fs_allowed(const struct capability_set *caps, const char *path, uint8_t rights) {
  static const struct {
    const char *path;
    uint8_t rights;
  } rules[] = {
    {"/tmp", CELL_FS_READ | CELL_FS_WRITE | CELL_FS_EXEC},
    {"/etc/ld-musl-aarch64.path", CELL_FS_READ},
    {"/lib", CELL_FS_READ | CELL_FS_EXEC},
    {"/usr/local/lib", CELL_FS_READ | CELL_FS_EXEC},
  };
  for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); ++i) {
    if ((rights & ~rules[i].rights) == 0 && under_path(path, rules[i].path)) { return true; }
  }
  return caps->executable[0] && !(rights & ~(CELL_FS_READ | CELL_FS_EXEC)) && under_path(path, caps->executable);
}

static bool caps_subset(const struct capability_set *requested, const struct capability_set *parent) {
  for (size_t i = 0; i < sizeof(requested->syscall_allow) / sizeof(requested->syscall_allow[0]); ++i) {
    if ((requested->syscall_allow[i] & ~parent->syscall_allow[i]) != 0) { return false; }
  }
  if (parent->restrict_egress) {
    if (!requested->restrict_egress) { return false; }
    const struct egress_rule *child = &requested->egress, *limit = &parent->egress;
    if (child->protocol != 0 &&
        (child->protocol != limit->protocol || child->port != limit->port ||
         (child->mask & limit->mask) != limit->mask || (child->address & limit->mask) != limit->address)) {
      return false;
    }
  }
  if (parent->memory_page_cap != 0 &&
      (requested->memory_page_cap == 0 || requested->memory_page_cap > parent->memory_page_cap)) {
    return false;
  }
  return !parent->restrict_fs ||
         (requested->restrict_fs &&
          (!requested->executable[0] || fs_allowed(parent, requested->executable, CELL_FS_READ | CELL_FS_EXEC)));
}

static void cap_allow(struct capability_set *caps, uint64_t nr) {
  if (nr < 512) { caps->syscall_allow[nr / 64] |= 1ull << (nr % 64); }
}

static void cap_allow_common(struct capability_set *caps) {
  static const uint16_t common[] = {
    17,  19,  23,  24,  25,  29,  57,  59,  63,  64,  65,  66,  72,  73,  80,  93,  94,  96,  98,  99,  101,
    103, 48,  113, 115, 123, 124, 134, 135, 144, 146, 160, 161, 172, 173, 174, 175, 176, 177, 178, 179, 198,
    200, 203, 204, 206, 207, 208, 209, 211, 212, 214, 215, 216, 220, 221, 222, 226, 233, 260, 261, 278, 439,
  };
  for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); ++i) {
    cap_allow(caps, common[i]);
  }
}

static void cap_allow_files(struct capability_set *caps) {
  static const uint16_t files[] = {34, 35, 38, 46, 49, 50, 52, 53, 54, 55, 56, 61, 62, 78, 79, 82, 276, 291};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
    cap_allow(caps, files[i]);
  }
}

bool cell_fs_path_allowed(const char *path, uint8_t rights) {
  const struct domain *domain = cell_current_domain_internal();
  return domain == NULL || domain->caps == NULL || !domain->caps->restrict_fs || fs_allowed(domain->caps, path, rights);
}

bool cell_fs_exec_fd_allowed(const char *path, const struct vfs_node *node) {
  const struct domain *domain = cell_current_domain_internal();
  if (!domain->caps || !domain->caps->restrict_fs) { return true; }
  struct vfs_node named;
  return fs_allowed(domain->caps, path, CELL_FS_EXEC) && vfs_lookup(path, &named) &&
         named.dev_id == node->dev_id && named.ino == node->ino;
}

bool cell_syscall_allowed(uint64_t nr) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || domain->caps == NULL) { return true; }
  if (nr >= 512) { return false; }
  return (domain->caps->syscall_allow[nr / 64] & (1ull << (nr % 64))) != 0;
}

bool cell_egress_allowed(uint8_t proto, uint32_t ip, uint16_t port) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || domain->caps == NULL || !domain->caps->restrict_egress) { return true; }
  const struct egress_rule *rule = &domain->caps->egress;
  bool allowed = rule->protocol == proto && rule->port == port && (ip & rule->mask) == rule->address;
  if (!allowed) {
    kprintf("[spore] egress denied domain=%d proto=%u dst=%x:%u\n", domain->id, (unsigned)proto, (unsigned)ip,
            (unsigned)port);
  }
  return allowed;
}

static uint64_t page_count(uint64_t start, uint64_t end) {
  return end > start ? (end - start) / 4096u : 0;
}

static uint64_t overlap_pages(const struct vma_list *vmas, uint64_t start, uint64_t end) {
  if (vmas == NULL || end <= start) { return 0; }
  uint64_t total = 0;
  for (size_t i = vma_first_ending_after(vmas, start); i < vma_count(vmas); ++i) {
    const struct vma *vma = vma_at(vmas, i);
    if (vma->start >= end) { break; }
    uint64_t overlap_start = start > vma->start ? start : vma->start;
    uint64_t overlap_end = end < vma->end ? end : vma->end;
    total += page_count(overlap_start, overlap_end);
  }
  return total;
}

static bool memory_pages_allowed(uint64_t removed_pages, uint64_t added_pages) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || domain->caps == NULL || domain->caps->memory_page_cap == 0) { return true; }

  const struct vma_list *vmas = cell_domain_vmas_const(domain);
  if (vmas == NULL) { return false; }
  uint64_t current_pages = vma_virtual_pages(vmas);
  uint64_t base_pages = removed_pages > current_pages ? 0 : current_pages - removed_pages;
  return added_pages <= domain->caps->memory_page_cap && base_pages <= domain->caps->memory_page_cap - added_pages;
}

bool cell_map_add_allowed(uint64_t start, uint64_t end, bool replace_existing) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || domain->caps == NULL || domain->caps->memory_page_cap == 0) { return true; }
  const struct vma_list *vmas = cell_domain_vmas_const(domain);
  uint64_t removed_pages = replace_existing ? overlap_pages(vmas, start, end) : 0;
  return memory_pages_allowed(removed_pages, page_count(start, end));
}

bool cell_map_resize_allowed(uint64_t old_start, uint64_t old_end, uint64_t new_start, uint64_t new_end,
                             bool replace_new) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL || domain->caps == NULL || domain->caps->memory_page_cap == 0) { return true; }
  const struct vma_list *vmas = cell_domain_vmas_const(domain);
  uint64_t removed_pages = overlap_pages(vmas, old_start, old_end);
  if (replace_new) { removed_pages += overlap_pages(vmas, new_start, new_end); }
  return memory_pages_allowed(removed_pages, page_count(new_start, new_end));
}

int cell_apply_policy(const char *manifest) {
  struct domain *domain = cell_current_domain_internal();
  if (domain == NULL) { return -3; }
  struct policy_request request;
  if (!spore_parse_manifest(manifest, &request)) { return -2; }
  struct capability_set caps = {.refs = 1};
  cap_allow_common(&caps);
  switch (request.profile) {
  case POLICY_COMPUTE:
    break;
  case POLICY_FILES:
    cap_allow_files(&caps);
    caps.restrict_fs = true;
    if (request.executable != NULL) {
      size_t len = kstrlen(request.executable);
      if (len >= sizeof(caps.executable)) { return -2; }
      kmemcpy(caps.executable, request.executable, len + 1);
    }
    break;
  case POLICY_MEMORY:
    caps.memory_page_cap = 1;
    break;
  case POLICY_NETWORK:
    cap_allow_files(&caps);
    caps.restrict_egress = true;
    caps.egress = request.network;
    break;
  }
  if (domain->caps != NULL && !caps_subset(&caps, domain->caps)) { return -1; }
  struct capability_set *published = pool_alloc(&policies);
  if (published == NULL) { return -12; }
  *published = caps;
  if (request.profile == POLICY_COMPUTE && (domain->budget.max_ticks == 0 || domain->budget.max_ticks > 20)) {
    if (domain->budget.max_ticks == 0 || domain->budget.remaining_ticks > 20) { domain->budget.remaining_ticks = 20; }
    domain->budget.max_ticks = 20;
  }
  cell_policy_release(domain->caps);
  domain->caps = published;
  kprintf("[spore] policy applied: %s\n", manifest);
  return 0;
}
