#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

enum exit_code {
  EXIT_USAGE = 64,
};

int eprintf(const char *fmt, ...);
int usage(const char *tool, const char *usage);
const char *basename(const char *path);
bool streq(const char *a, const char *b);

struct user_entry {
  char name[32];
  char password[64];
  unsigned uid;
  unsigned gid;
  char gecos[64];
  char home[128];
  char shell[128];
};

struct group_entry {
  char name[32];
  unsigned gid;
};

bool user_by_name(const char *name, struct user_entry *out);
bool user_by_uid(unsigned uid, struct user_entry *out);
bool group_by_name(const char *name, struct group_entry *out);
bool group_by_gid(unsigned gid, struct group_entry *out);
unsigned next_user_id(void);
bool password_matches(const char *name, const char *password);
bool set_shadow_password(const char *name, const char *password);
bool add_shadow_user(const char *name);
bool remove_shadow_user(const char *name);
bool sudo_user_allowed(const char *name, bool *nopasswd);

enum {
  SYS_spore_procinfo = 0x4007,
  SYS_spore_fsinfo = 0x4008,
  SYS_spore_mountinfo = 0x4009,
  SYS_spore_net_config = 0x400a,
};

struct net_config {
  uint32_t local_ip;
  uint32_t gateway_ip;
  uint32_t netmask;
  uint32_t dns_ip;
  uint32_t configured;
};

struct proc_info {
  uint32_t pid;
  uint32_t tid;
  uint32_t ppid;
  uint32_t state;
  uint32_t wait_reason;
  uint32_t _pad;
  uint64_t resident_pages;
  uint64_t remaining_ticks;
  uint64_t max_ticks;
  char cwd[64];
};

struct fs_info {
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
  uint64_t inode_count;
  uint64_t free_inodes;
};

struct mount_info {
  char source[32];
  char target[32];
  char fstype[16];
  uint64_t block_size;
  uint64_t block_count;
  uint64_t free_blocks;
};

bool parse_ipv4(const char *s, uint32_t *out);
void format_ipv4(uint32_t ip, char *out, size_t cap);
bool resolve_ipv4(const char *name, uint32_t *out);
bool net_config_get(struct net_config *out);
bool net_config_set(const struct net_config *cfg);
