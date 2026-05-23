#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    VMM_USER_READ = 1u << 0,
    VMM_USER_WRITE = 1u << 1,
    VMM_USER_EXEC = 1u << 2,
};

enum vmm_access {
    VMM_ACCESS_READ,
    VMM_ACCESS_WRITE,
    VMM_ACCESS_EXEC,
};

struct user_address_space {
    uint64_t root_pa;
    uint64_t hhdm_offset;
    uint64_t brk_base;
    uint64_t brk_current;
    uint64_t mmap_base;
    uint16_t asid;
};

bool vmm_user_init(struct user_address_space *as, uint64_t hhdm_offset);
bool vmm_map_page(struct user_address_space *as, uint64_t va, uint64_t pa, uint32_t flags);
bool vmm_alloc_page(struct user_address_space *as, uint64_t va, uint32_t flags);
bool vmm_clone_cow(struct user_address_space *dst,
                   struct user_address_space *src,
                   uint16_t asid);
void vmm_destroy(struct user_address_space *as);
bool vmm_handle_cow_fault(struct user_address_space *as, uint64_t va);
uint64_t vmm_user_to_phys(const struct user_address_space *as, uint64_t va);
bool vmm_user_range_accessible(const struct user_address_space *as,
                               uint64_t va,
                               size_t len,
                               enum vmm_access access);
bool vmm_copy_to_user(const struct user_address_space *as, uint64_t dst, const void *src, size_t len);
bool vmm_copy_from_user(const struct user_address_space *as, void *dst, uint64_t src, size_t len);
void vmm_install_user(const struct user_address_space *as);
void vmm_enable_ttbr0(void);
void vmm_flush_user_va(uint64_t va);
