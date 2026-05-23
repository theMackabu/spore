#include "mm/vmm.h"

#include "mem.h"
#include "mm/pmm.h"

enum {
    PTE_VALID = 1ull << 0,
    PTE_TABLE = 1ull << 1,
    PTE_AF = 1ull << 10,
    PTE_SH_INNER = 3ull << 8,
    PTE_AP_USER_RW = 1ull << 6,
    PTE_AP_USER_RO = 3ull << 6,
};

#define PTE_PXN (1ull << 53)
#define PTE_UXN (1ull << 54)
#define PTE_ADDR_MASK 0x0000fffffffff000ull

static uint64_t *pt_virt(const struct user_address_space *as, uint64_t pa) {
    return (uint64_t *)(uintptr_t)(as->hhdm_offset + pa);
}

static size_t pt_index(uint64_t va, unsigned shift) {
    return (size_t)((va >> shift) & 0x1ff);
}

static bool checked_add(uint64_t a, uint64_t b, uint64_t *out) {
    *out = a + b;
    return *out >= a;
}

bool vmm_user_init(struct user_address_space *as, uint64_t hhdm_offset) {
    uint64_t root = pmm_alloc_zero_page();
    if (root == 0) {
        return false;
    }
    as->root_pa = root;
    as->hhdm_offset = hhdm_offset;
    as->brk_base = 0;
    as->brk_current = 0;
    as->mmap_base = 0x0000007000000000ull;
    return true;
}

static bool ensure_table(struct user_address_space *as, uint64_t *table, size_t index, uint64_t **next) {
    uint64_t entry = table[index];
    if ((entry & PTE_VALID) == 0) {
        uint64_t pa = pmm_alloc_zero_page();
        if (pa == 0) {
            return false;
        }
        table[index] = pa | PTE_VALID | PTE_TABLE;
        entry = table[index];
    }
    if ((entry & PTE_TABLE) == 0) {
        return false;
    }
    *next = pt_virt(as, entry & PTE_ADDR_MASK);
    return true;
}

bool vmm_map_page(struct user_address_space *as, uint64_t va, uint64_t pa, uint32_t flags) {
    uint64_t *l0 = pt_virt(as, as->root_pa);
    uint64_t *l1;
    uint64_t *l2;
    uint64_t *l3;

    if (!ensure_table(as, l0, pt_index(va, 39), &l1) ||
        !ensure_table(as, l1, pt_index(va, 30), &l2) ||
        !ensure_table(as, l2, pt_index(va, 21), &l3)) {
        return false;
    }

    uint64_t ap = (flags & VMM_USER_WRITE) != 0 ? PTE_AP_USER_RW : PTE_AP_USER_RO;
    uint64_t xn = (flags & VMM_USER_EXEC) != 0 ? 0 : PTE_UXN;
    l3[pt_index(va, 12)] = (pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE |
                           PTE_AF | PTE_SH_INNER | ap | PTE_PXN | xn;
    return true;
}

bool vmm_alloc_page(struct user_address_space *as, uint64_t va, uint32_t flags) {
    uint64_t pa = pmm_alloc_zero_page();
    return pa != 0 && vmm_map_page(as, va, pa, flags);
}

static bool user_entry_allows(uint64_t entry, enum vmm_access access) {
    switch (access) {
    case VMM_ACCESS_READ:
        return true;
    case VMM_ACCESS_WRITE:
        return (entry & PTE_AP_USER_RO) != PTE_AP_USER_RO;
    case VMM_ACCESS_EXEC:
        return (entry & PTE_UXN) == 0;
    }
    return false;
}

static uint64_t user_to_phys_checked(const struct user_address_space *as,
                                     uint64_t va,
                                     enum vmm_access access) {
    const uint64_t *l0 = pt_virt(as, as->root_pa);
    uint64_t entry = l0[pt_index(va, 39)];
    if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) {
        return 0;
    }
    const uint64_t *l1 = pt_virt(as, entry & PTE_ADDR_MASK);
    entry = l1[pt_index(va, 30)];
    if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) {
        return 0;
    }
    const uint64_t *l2 = pt_virt(as, entry & PTE_ADDR_MASK);
    entry = l2[pt_index(va, 21)];
    if ((entry & (PTE_VALID | PTE_TABLE)) != (PTE_VALID | PTE_TABLE)) {
        return 0;
    }
    const uint64_t *l3 = pt_virt(as, entry & PTE_ADDR_MASK);
    entry = l3[pt_index(va, 12)];
    if ((entry & PTE_VALID) == 0) {
        return 0;
    }
    if (!user_entry_allows(entry, access)) {
        return 0;
    }
    return (entry & PTE_ADDR_MASK) | (va & 0xfff);
}

uint64_t vmm_user_to_phys(const struct user_address_space *as, uint64_t va) {
    return user_to_phys_checked(as, va, VMM_ACCESS_READ);
}

bool vmm_user_range_accessible(const struct user_address_space *as,
                               uint64_t va,
                               size_t len,
                               enum vmm_access access) {
    if (len == 0) {
        return true;
    }
    uint64_t end;
    if (!checked_add(va, len - 1, &end)) {
        return false;
    }

    uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = end & ~(uint64_t)(PAGE_SIZE - 1);
    for (;;) {
        if (user_to_phys_checked(as, page, access) == 0) {
            return false;
        }
        if (page == last) {
            return true;
        }
        page += PAGE_SIZE;
    }
}

bool vmm_copy_to_user(const struct user_address_space *as, uint64_t dst, const void *src, size_t len) {
    if (!vmm_user_range_accessible(as, dst, len, VMM_ACCESS_WRITE)) {
        return false;
    }
    const uint8_t *s = src;
    for (size_t i = 0; i < len; ++i) {
        uint64_t pa = user_to_phys_checked(as, dst + i, VMM_ACCESS_WRITE);
        *(uint8_t *)(uintptr_t)(as->hhdm_offset + pa) = s[i];
    }
    return true;
}

bool vmm_copy_from_user(const struct user_address_space *as, void *dst, uint64_t src, size_t len) {
    if (!vmm_user_range_accessible(as, src, len, VMM_ACCESS_READ)) {
        return false;
    }
    uint8_t *d = dst;
    for (size_t i = 0; i < len; ++i) {
        uint64_t pa = user_to_phys_checked(as, src + i, VMM_ACCESS_READ);
        d[i] = *(uint8_t *)(uintptr_t)(as->hhdm_offset + pa);
    }
    return true;
}

void vmm_enable_ttbr0(void) {
    uint64_t tcr;
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
    const uint64_t clear = 0x3full | (1ull << 7) | (3ull << 8) | (3ull << 10) |
                           (3ull << 12) | (3ull << 14);
    tcr &= ~clear;
    tcr |= 16ull | (1ull << 8) | (1ull << 10) | (3ull << 12);
    __asm__ volatile(
        "msr tcr_el1, %0\n"
        "isb\n"
        :
        : "r"(tcr)
        : "memory");
}

void vmm_install_user(const struct user_address_space *as) {
    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "dsb ishst\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :
        : "r"(as->root_pa)
        : "memory");
}
