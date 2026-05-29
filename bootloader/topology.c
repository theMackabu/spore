#include "bootloader.h"

#include <stdbool.h>

struct acpi_rsdp {
  char signature[8];
  uint8_t checksum;
  char oem_id[6];
  uint8_t revision;
  uint32_t rsdt_address;
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t extended_checksum;
  uint8_t reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
  char signature[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  char oem_id[6];
  char oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
  struct acpi_sdt_header hdr;
  uint32_t local_interrupt_controller_address;
  uint32_t flags;
  uint8_t entries[];
} __attribute__((packed));

struct acpi_madt_gicc {
  uint8_t type;
  uint8_t length;
  uint16_t reserved;
  uint32_t cpu_interface_number;
  uint32_t acpi_processor_uid;
  uint32_t flags;
  uint32_t parking_protocol_version;
  uint32_t performance_interrupt_gsiv;
  uint64_t parked_address;
  uint64_t physical_base_address;
  uint64_t gicv;
  uint64_t gich;
  uint32_t vgic_maintenance_interrupt;
  uint64_t gicr_base_address;
  uint64_t mpidr;
} __attribute__((packed));

static bool guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
  if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3) { return false; }
  for (uint32_t i = 0; i < sizeof(a->data4); ++i) {
    if (a->data4[i] != b->data4[i]) { return false; }
  }
  return true;
}

static bool sig_eq(const char *sig, const char *want, uint32_t len) {
  for (uint32_t i = 0; i < len; ++i) {
    if (sig[i] != want[i]) { return false; }
  }
  return true;
}

static uint64_t current_mpidr(void) {
  uint64_t mpidr;
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  return mpidr & 0xff00ffffffull;
}

static bool add_cpu(struct spore_cpu_entry *out, uint32_t cap, uint32_t *count, uint64_t mpidr, uint32_t flags) {
  mpidr &= 0xff00ffffffull;
  for (uint32_t i = 0; i < *count; ++i) {
    if (out[i].mpidr == mpidr) {
      out[i].flags |= flags;
      return true;
    }
  }
  if (*count >= cap) { return false; }
  out[*count] = (struct spore_cpu_entry){.mpidr = mpidr, .flags = flags, .reserved = 0};
  ++*count;
  return true;
}

static void make_boot_cpu_first(struct spore_cpu_entry *out, uint32_t *count, uint32_t cap, uint64_t boot_mpidr) {
  boot_mpidr &= 0xff00ffffffull;
  for (uint32_t i = 0; i < *count; ++i) {
    if (out[i].mpidr != boot_mpidr) { continue; }
    out[i].flags |= SPORE_CPU_BOOT;
    if (i != 0) {
      struct spore_cpu_entry tmp = out[0];
      out[0] = out[i];
      out[i] = tmp;
    }
    return;
  }
  if (*count < cap) {
    for (uint32_t i = *count; i > 0; --i) {
      out[i] = out[i - 1];
    }
    out[0] = (struct spore_cpu_entry){.mpidr = boot_mpidr, .flags = SPORE_CPU_PRESENT | SPORE_CPU_BOOT};
    ++*count;
  }
}

static struct acpi_rsdp *find_rsdp(void) {
  if (st == NULL || st->configuration_table == NULL) { return NULL; }
  for (UINTN i = 0; i < st->number_of_table_entries; ++i) {
    EFI_CONFIGURATION_TABLE *table = &st->configuration_table[i];
    if (guid_eq(&table->vendor_guid, &EFI_ACPI_20_TABLE_GUID) ||
        guid_eq(&table->vendor_guid, &EFI_ACPI_10_TABLE_GUID)) {
      struct acpi_rsdp *rsdp = table->vendor_table;
      if (rsdp != NULL && sig_eq(rsdp->signature, "RSD PTR ", 8)) { return rsdp; }
    }
  }
  return NULL;
}

static struct acpi_sdt_header *find_sdt_from_xsdt(struct acpi_sdt_header *xsdt, const char *sig) {
  if (xsdt == NULL || !sig_eq(xsdt->signature, "XSDT", 4) || xsdt->length < sizeof(*xsdt)) { return NULL; }
  uint32_t count = (xsdt->length - sizeof(*xsdt)) / sizeof(uint64_t);
  uint64_t *entries = (uint64_t *)(uintptr_t)((uint8_t *)xsdt + sizeof(*xsdt));
  for (uint32_t i = 0; i < count; ++i) {
    struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)(uintptr_t)entries[i];
    if (hdr != NULL && sig_eq(hdr->signature, sig, 4)) { return hdr; }
  }
  return NULL;
}

static struct acpi_sdt_header *find_sdt_from_rsdt(struct acpi_sdt_header *rsdt, const char *sig) {
  if (rsdt == NULL || !sig_eq(rsdt->signature, "RSDT", 4) || rsdt->length < sizeof(*rsdt)) { return NULL; }
  uint32_t count = (rsdt->length - sizeof(*rsdt)) / sizeof(uint32_t);
  uint32_t *entries = (uint32_t *)(uintptr_t)((uint8_t *)rsdt + sizeof(*rsdt));
  for (uint32_t i = 0; i < count; ++i) {
    struct acpi_sdt_header *hdr = (struct acpi_sdt_header *)(uintptr_t)(uint64_t)entries[i];
    if (hdr != NULL && sig_eq(hdr->signature, sig, 4)) { return hdr; }
  }
  return NULL;
}

static void parse_madt(struct acpi_madt *madt, struct spore_cpu_entry *out, uint32_t cap, uint32_t *count) {
  if (madt == NULL || !sig_eq(madt->hdr.signature, "APIC", 4) || madt->hdr.length < sizeof(*madt)) { return; }
  uint8_t *ptr = madt->entries;
  uint8_t *end = (uint8_t *)madt + madt->hdr.length;
  while (ptr + 2 <= end) {
    uint8_t type = ptr[0];
    uint8_t len = ptr[1];
    if (len < 2 || ptr + len > end) { break; }
    if (type == 0x0b && len >= sizeof(struct acpi_madt_gicc)) {
      struct acpi_madt_gicc *gicc = (struct acpi_madt_gicc *)ptr;
      if ((gicc->flags & 1u) != 0) {
        (void)add_cpu(out, cap, count, gicc->mpidr, SPORE_CPU_PRESENT);
      }
    }
    ptr += len;
  }
}

uint32_t discover_cpu_topology(struct spore_cpu_entry *out, uint32_t cap) {
  if (out == NULL || cap == 0) { return 0; }
  uint32_t count = 0;
  struct acpi_rsdp *rsdp = find_rsdp();
  if (rsdp != NULL) {
    struct acpi_sdt_header *madt = NULL;
    if (rsdp->revision >= 2 && rsdp->xsdt_address != 0) {
      madt = find_sdt_from_xsdt((struct acpi_sdt_header *)(uintptr_t)rsdp->xsdt_address, "APIC");
    }
    if (madt == NULL && rsdp->rsdt_address != 0) {
      madt = find_sdt_from_rsdt((struct acpi_sdt_header *)(uintptr_t)(uint64_t)rsdp->rsdt_address, "APIC");
    }
    parse_madt((struct acpi_madt *)madt, out, cap, &count);
  }
  make_boot_cpu_first(out, &count, cap, current_mpidr());
  return count;
}
