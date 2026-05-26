#include "bootloader.h"

EFI_STATUS open_root(EFI_HANDLE image) {
  EFI_LOADED_IMAGE_PROTOCOL *loaded = NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
  EFI_STATUS status = bs->handle_protocol(image, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded);
  if (EFI_ERROR(status)) { return status; }
  status = bs->handle_protocol(loaded->device_handle, (EFI_GUID *)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID, (void **)&fs);
  if (EFI_ERROR(status)) { return status; }
  return fs->open_volume(fs, &root);
}

static EFI_STATUS file_size(EFI_FILE_PROTOCOL *file, uint64_t *size) {
  uint8_t info_buf[512];
  UINTN info_size = sizeof(info_buf);
  EFI_STATUS status = file->get_info(file, (EFI_GUID *)&EFI_FILE_INFO_GUID, &info_size, info_buf);
  if (EFI_ERROR(status)) { return status; }
  EFI_FILE_INFO *info = (EFI_FILE_INFO *)info_buf;
  *size = info->file_size;
  return EFI_SUCCESS;
}

EFI_STATUS read_file(CHAR16 *path, struct loaded_file *out) {
  EFI_FILE_PROTOCOL *file = NULL;
  EFI_STATUS status = root->open(root, &file, path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR(status)) { return status; }
  uint64_t size = 0;
  status = file_size(file, &size);
  if (EFI_ERROR(status)) {
    file->close(file);
    return status;
  }
  void *data = NULL;
  status = alloc_pages(pages_for(size == 0 ? 1 : size), &data);
  if (EFI_ERROR(status)) {
    file->close(file);
    return status;
  }
  UINTN read_size = (UINTN)size;
  status = file->read(file, &read_size, data);
  file->close(file);
  if (EFI_ERROR(status) || read_size != size) { return EFI_LOAD_ERROR; }
  out->data = data;
  out->size = size;
  out->phys = (uint64_t)(uintptr_t)data;
  return EFI_SUCCESS;
}

void ascii_to_efi_path(const char *src, CHAR16 *dst, uint64_t cap) {
  uint64_t j = 0;
  for (uint64_t i = 0; src[i] != '\0' && j + 1 < cap; ++i) {
    char c = src[i] == '/' ? '\\' : src[i];
    dst[j++] = (CHAR16)c;
  }
  dst[j] = 0;
}

static int next_line(char **cursor, char *line, uint64_t cap) {
  char *p = *cursor;
  if (*p == '\0') { return 0; }
  uint64_t n = 0;
  while (*p != '\0' && *p != '\n' && *p != '\r') {
    if (n + 1 < cap) { line[n++] = *p; }
    ++p;
  }
  while (*p == '\n' || *p == '\r') {
    ++p;
  }
  line[n] = '\0';
  *cursor = p;
  return 1;
}

static int split_manifest_line(char *line, char **esp_path, char **target_path) {
  char *p = line;
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  if (*p == '\0' || *p == '#') { return 0; }
  *esp_path = p;
  while (*p != '\0' && *p != ' ' && *p != '\t') {
    ++p;
  }
  if (*p == '\0') { return -1; }
  *p++ = '\0';
  while (*p == ' ' || *p == '\t') {
    ++p;
  }
  if (*p == '\0') { return -1; }
  *target_path = p;
  while (*p != '\0' && *p != ' ' && *p != '\t') {
    ++p;
  }
  *p = '\0';
  return 1;
}

EFI_STATUS load_modules(struct spore_boot_module *modules, uint32_t *count) {
  struct loaded_file manifest;
  EFI_STATUS status = read_file(u"\\boot\\modules.txt", &manifest);
  if (EFI_ERROR(status)) { return status; }
  char *cursor = manifest.data;
  char line[256];
  uint32_t n = 0;
  while (next_line(&cursor, line, sizeof(line))) {
    char *esp_path = NULL;
    char *target_path = NULL;
    int split = split_manifest_line(line, &esp_path, &target_path);
    if (split == 0) { continue; }
    if (split < 0 || n >= MAX_MODULES) { return EFI_LOAD_ERROR; }
    CHAR16 efi_path[160];
    ascii_to_efi_path(esp_path, efi_path, sizeof(efi_path) / sizeof(efi_path[0]));
    struct loaded_file file;
    status = read_file(efi_path, &file);
    if (EFI_ERROR(status)) { return status; }
    modules[n].phys_addr = file.phys;
    modules[n].size = file.size;
    uint64_t len = strlen8(target_path);
    if (len >= SPORE_BOOT_MODULE_PATH_MAX) { return EFI_LOAD_ERROR; }
    for (uint64_t i = 0; i <= len; ++i) {
      modules[n].path[i] = target_path[i];
    }
    ++n;
  }
  *count = n;
  return EFI_SUCCESS;
}
