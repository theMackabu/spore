#include "kprintf.h"
#include "arch/aarch64/exceptions.h"
#include "cell.h"
#include "elf/loader.h"
#include "exec/stack.h"
#include "mem.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "pl011.h"
#include "ramfs.h"

#include <limine.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[4] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_stack_size_request stack_size_request = {
    .id = LIMINE_STACK_SIZE_REQUEST_ID,
    .revision = 0,
    .stack_size = 64 * 1024,
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[2] = LIMINE_REQUESTS_END_MARKER;

void kputc(char c) {
    pl011_putc(c);
}

void kputs(const char *s) {
    while (*s != '\0') {
        kputc(*s++);
    }
}

static void print_unsigned(uint64_t value, uint64_t base) {
    char buf[32];
    size_t i = 0;

    if (value == 0) {
        kputc('0');
        return;
    }

    while (value != 0) {
        uint64_t digit = value % base;
        buf[i++] = (char)(digit < 10 ? '0' + digit : 'a' + (digit - 10));
        value /= base;
    }
    while (i > 0) {
        kputc(buf[--i]);
    }
}

static void print_signed(int64_t value) {
    if (value < 0) {
        kputc('-');
        print_unsigned((uint64_t)-value, 10);
        return;
    }
    print_unsigned((uint64_t)value, 10);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p != '\0'; ++p) {
        if (*p != '%') {
            kputc(*p);
            continue;
        }

        char spec = *++p;
        switch (spec) {
        case '%':
            kputc('%');
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            kputs(s == NULL ? "(null)" : s);
            break;
        }
        case 'd':
            print_signed((int64_t)va_arg(ap, int));
            break;
        case 'u':
            print_unsigned((uint64_t)va_arg(ap, unsigned int), 10);
            break;
        case 'x':
            print_unsigned((uint64_t)va_arg(ap, unsigned int), 16);
            break;
        case 'p':
            kputs("0x");
            print_unsigned((uint64_t)(uintptr_t)va_arg(ap, void *), 16);
            break;
        default:
            kputc('%');
            kputc(spec);
            break;
        }
    }

    va_end(ap);
}

static uint64_t current_el(void) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 0x3;
}

enum {
    PL011_PHYS = 0x09000000,
    EARLY_PAGE_SIZE = 0x1000,
    PT_ENTRIES = 512,
};

static uint64_t early_l1[PT_ENTRIES] __attribute__((aligned(EARLY_PAGE_SIZE)));
static uint64_t early_l2[PT_ENTRIES] __attribute__((aligned(EARLY_PAGE_SIZE)));
static uint64_t early_l3[PT_ENTRIES] __attribute__((aligned(EARLY_PAGE_SIZE)));
static uint8_t kernel_stack[64 * 1024] __attribute__((aligned(16)));

static uint64_t kernel_virt_to_phys(uintptr_t va) {
    const struct limine_executable_address_response *executable =
        executable_address_request.response;
    return (uint64_t)va - executable->virtual_base + executable->physical_base;
}

static uint64_t *phys_to_hhdm(uint64_t pa) {
    return (uint64_t *)(uintptr_t)(hhdm_request.response->offset + pa);
}

static uint64_t pt_entry_address(uint64_t entry) {
    return entry & 0x0000fffffffff000ull;
}

static uint64_t table_descriptor(uint64_t *table) {
    return kernel_virt_to_phys((uintptr_t)table) | 0x3ull;
}

static void zero_page(uint64_t *page) {
    for (size_t i = 0; i < PT_ENTRIES; ++i) {
        page[i] = 0;
    }
}

static void map_pl011_page(void) {
    zero_page(early_l1);
    zero_page(early_l2);
    zero_page(early_l3);

    uint64_t mair;
    __asm__ volatile("mrs %0, mair_el1" : "=r"(mair));
    mair &= ~0xffull;
    mair |= 0xffull;
    mair &= ~(0xffull << 16);
    mair |= 0x00ull << 16;
    __asm__ volatile(
        "msr mair_el1, %0\n"
        "isb\n"
        :
        : "r"(mair)
        : "memory");

    const uint64_t va = hhdm_request.response->offset + PL011_PHYS;
    uint64_t ttbr1;
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));

    uint64_t *l0 = phys_to_hhdm(pt_entry_address(ttbr1));
    const size_t l0i = (va >> 39) & 0x1ff;
    const size_t l1i = (va >> 30) & 0x1ff;
    const size_t l2i = (va >> 21) & 0x1ff;
    const size_t l3i = (va >> 12) & 0x1ff;

    if ((l0[l0i] & 0x1) == 0) {
        l0[l0i] = table_descriptor(early_l1);
    }
    uint64_t *l1 = phys_to_hhdm(pt_entry_address(l0[l0i]));

    if ((l1[l1i] & 0x1) == 0) {
        l1[l1i] = table_descriptor(early_l2);
    }
    uint64_t *l2 = phys_to_hhdm(pt_entry_address(l1[l1i]));

    if ((l2[l2i] & 0x1) == 0) {
        l2[l2i] = table_descriptor(early_l3);
    }
    uint64_t *l3 = phys_to_hhdm(pt_entry_address(l2[l2i]));

    const uint64_t attr_index_device = 2ull << 2;
    const uint64_t ap_el1_rw = 0ull << 6;
    const uint64_t sh_inner = 3ull << 8;
    const uint64_t af = 1ull << 10;
    const uint64_t pxn = 1ull << 53;
    const uint64_t uxn = 1ull << 54;
    l3[l3i] = (PL011_PHYS & ~0xfffull) | attr_index_device | ap_el1_rw |
              sh_inner | af | pxn | uxn | 0x3ull;

    __asm__ volatile(
        "dsb ishst\n"
        "tlbi vmalle1\n"
        "dsb ish\n"
        "isb\n"
        :
        :
        : "memory");
}

void finish_enter_el0(struct user_address_space *as, uint64_t entry, uint64_t user_sp) {
    if (!cell_create_init(as, entry, user_sp)) {
        kprintf("[kernel] failed to create init cell\n");
        for (;;) {
            __asm__ volatile("wfe");
        }
    }
    syscall_set_address_space(cell_current_as());
    vmm_enable_ttbr0();
    vmm_install_user(cell_current_as());
    kprintf("[kernel] entering EL0\n");
    enter_el0(entry, user_sp);
}

void kernel_main(void) {
    if (hhdm_request.response == NULL || executable_address_request.response == NULL ||
        memmap_request.response == NULL ||
        !LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        for (;;) {
            __asm__ volatile("wfe");
        }
    }

    map_pl011_page();
    pl011_init(hhdm_request.response->offset);

    kprintf("[kernel] booted at EL%u\n", (unsigned)current_el());

    pmm_init(hhdm_request.response->offset, memmap_request.response);
    exceptions_init();
    cell_system_init(hhdm_request.response->offset);

    struct ramfs fs;
    struct ramfs_file init;
    ramfs_init(&fs, module_request.response);
    if (!ramfs_lookup(&fs, "/init", &init)) {
        kprintf("[kernel] missing /init\n");
        for (;;) {
            __asm__ volatile("wfe");
        }
    }
    kprintf("[kernel] loading /init\n");

    struct user_address_space as;
    struct loaded_elf elf;
    uint64_t user_sp;
    if (!vmm_user_init(&as, hhdm_request.response->offset) ||
        !elf_load_static_aarch64(&as, init.data, init.size, &elf) ||
        !build_initial_stack(&as, &elf, &user_sp)) {
        kprintf("[kernel] failed to prepare /init\n");
        for (;;) {
            __asm__ volatile("wfe");
        }
    }

    uint64_t kernel_sp = (uint64_t)(uintptr_t)(kernel_stack + sizeof(kernel_stack));
    switch_stack_and_finish(kernel_sp, &as, elf.entry, user_sp);

    for (;;) {
        __asm__ volatile("wfe");
    }
}
