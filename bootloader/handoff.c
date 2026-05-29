#include "bootloader.h"

static uint32_t efi_mem_type(uint32_t efi_type) {
  switch (efi_type) {
  case 7:
    return SPORE_MEMMAP_USABLE;
  case 9:
    return SPORE_MEMMAP_ACPI_RECLAIMABLE;
  case 10:
    return SPORE_MEMMAP_ACPI_NVS;
  case 11:
  case 12:
  case 13:
    return SPORE_MEMMAP_MMIO;
  case 3:
  case 4:
    return SPORE_MEMMAP_BOOTLOADER_RECLAIMABLE;
  default:
    return SPORE_MEMMAP_RESERVED;
  }
}

EFI_STATUS memory_map_highest_usable(EFI_MEMORY_DESCRIPTOR *efi_map, UINTN efi_map_capacity, uint64_t *highest_out) {
  UINTN map_size = efi_map_capacity;
  UINTN map_key = 0;
  UINTN desc_size = 0;
  UINT32 desc_version = 0;
  EFI_STATUS status = bs->get_memory_map(&map_size, efi_map, &map_key, &desc_size, &desc_version);
  if (status == EFI_BUFFER_TOO_SMALL) { return status; }
  if (EFI_ERROR(status)) { return status; }

  uint64_t highest = 0;
  uint32_t count = (uint32_t)(map_size / desc_size);
  for (uint32_t i = 0; i < count; ++i) {
    EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)efi_map + (uint64_t)i * desc_size);
    if (efi_mem_type(desc->type) != SPORE_MEMMAP_USABLE) { continue; }
    uint64_t end = desc->physical_start + desc->number_of_pages * PAGE_SIZE;
    if (end > highest) { highest = end; }
  }
  *highest_out = highest;
  return EFI_SUCCESS;
}

EFI_STATUS final_memory_map(struct spore_memmap_entry *out, uint32_t *out_count, EFI_MEMORY_DESCRIPTOR *efi_map,
                            UINTN efi_map_capacity, EFI_HANDLE image) {
  for (;;) {
    UINTN map_size = efi_map_capacity;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_version = 0;
    EFI_STATUS status = bs->get_memory_map(&map_size, efi_map, &map_key, &desc_size, &desc_version);
    if (status == EFI_BUFFER_TOO_SMALL) { return status; }
    if (EFI_ERROR(status)) { return status; }
    uint32_t count = (uint32_t)(map_size / desc_size);
    if (count > MAX_MEMMAP) { return EFI_LOAD_ERROR; }
    for (uint32_t i = 0; i < count; ++i) {
      EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)efi_map + (uint64_t)i * desc_size);
      out[i].base = desc->physical_start;
      out[i].length = desc->number_of_pages * PAGE_SIZE;
      out[i].type = efi_mem_type(desc->type);
      out[i].reserved = 0;
    }
    *out_count = count;
    status = bs->exit_boot_services(image, map_key);
    if (status == EFI_INVALID_PARAMETER) { continue; }
    return status;
  }
}
