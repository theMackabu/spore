#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint16_t CHAR16;
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;
typedef uint64_t UINTN;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint8_t UINT8;
typedef uint16_t UINT16;

#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR 0x8000000000000001ull
#define EFI_INVALID_PARAMETER 0x8000000000000002ull
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ull

#define EFI_FILE_MODE_READ 0x0000000000000001ull
#define EFI_ALLOCATE_ANY_PAGES 0
#define EFI_LOADER_DATA 2

#define EFI_ERROR(status) (((status) & 0x8000000000000000ull) != 0)

typedef struct {
  UINT32 data1;
  UINT16 data2;
  UINT16 data3;
  UINT8 data4[8];
} EFI_GUID;

typedef struct {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;
  uint32_t crc32;
  uint32_t reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
  void *reset;
  EFI_STATUS (*output_string)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *self, CHAR16 *string);
};

typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;

typedef struct {
  uint32_t type;
  EFI_PHYSICAL_ADDRESS physical_start;
  EFI_VIRTUAL_ADDRESS virtual_start;
  uint64_t number_of_pages;
  uint64_t attribute;
} EFI_MEMORY_DESCRIPTOR;

struct EFI_BOOT_SERVICES {
  EFI_TABLE_HEADER hdr;
  void *raise_tpl;
  void *restore_tpl;
  EFI_STATUS (*allocate_pages)(int type, int memory_type, UINTN pages, EFI_PHYSICAL_ADDRESS *memory);
  EFI_STATUS (*free_pages)(EFI_PHYSICAL_ADDRESS memory, UINTN pages);
  EFI_STATUS (*get_memory_map)(UINTN *memory_map_size, EFI_MEMORY_DESCRIPTOR *memory_map, UINTN *map_key,
                               UINTN *descriptor_size, UINT32 *descriptor_version);
  EFI_STATUS (*allocate_pool)(int pool_type, UINTN size, void **buffer);
  EFI_STATUS (*free_pool)(void *buffer);
  void *create_event;
  void *set_timer;
  void *wait_for_event;
  void *signal_event;
  void *close_event;
  void *check_event;
  void *install_protocol_interface;
  void *reinstall_protocol_interface;
  void *uninstall_protocol_interface;
  EFI_STATUS (*handle_protocol)(EFI_HANDLE handle, EFI_GUID *protocol, void **interface);
  void *reserved;
  void *register_protocol_notify;
  void *locate_handle;
  void *locate_device_path;
  void *install_configuration_table;
  void *load_image;
  void *start_image;
  void *exit;
  void *unload_image;
  EFI_STATUS (*exit_boot_services)(EFI_HANDLE image_handle, UINTN map_key);
};

struct EFI_SYSTEM_TABLE {
  EFI_TABLE_HEADER hdr;
  CHAR16 *firmware_vendor;
  uint32_t firmware_revision;
  EFI_HANDLE console_in_handle;
  void *con_in;
  EFI_HANDLE console_out_handle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *con_out;
  EFI_HANDLE standard_error_handle;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *std_err;
  void *runtime_services;
  EFI_BOOT_SERVICES *boot_services;
};

typedef struct {
  uint32_t revision;
  EFI_HANDLE parent_handle;
  EFI_SYSTEM_TABLE *system_table;
  EFI_HANDLE device_handle;
  void *file_path;
  void *reserved;
  uint32_t load_options_size;
  void *load_options;
  void *image_base;
  uint64_t image_size;
  int image_code_type;
  int image_data_type;
  EFI_STATUS (*unload)(EFI_HANDLE image_handle);
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
  uint64_t revision;
  EFI_STATUS (*open)(EFI_FILE_PROTOCOL *self, EFI_FILE_PROTOCOL **new_handle, CHAR16 *file_name, uint64_t open_mode,
                     uint64_t attributes);
  EFI_STATUS (*close)(EFI_FILE_PROTOCOL *self);
  EFI_STATUS (*delete_file)(EFI_FILE_PROTOCOL *self);
  EFI_STATUS (*read)(EFI_FILE_PROTOCOL *self, UINTN *buffer_size, void *buffer);
  EFI_STATUS (*write)(EFI_FILE_PROTOCOL *self, UINTN *buffer_size, void *buffer);
  EFI_STATUS (*get_position)(EFI_FILE_PROTOCOL *self, uint64_t *position);
  EFI_STATUS (*set_position)(EFI_FILE_PROTOCOL *self, uint64_t position);
  EFI_STATUS (*get_info)(EFI_FILE_PROTOCOL *self, EFI_GUID *information_type, UINTN *buffer_size, void *buffer);
};

typedef struct {
  uint64_t revision;
  EFI_STATUS (*open_volume)(void *self, EFI_FILE_PROTOCOL **root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
  uint64_t size;
  uint64_t file_size;
  uint64_t physical_size;
  uint64_t create_time[3];
  uint64_t last_access_time[3];
  uint64_t modification_time[3];
  uint64_t attribute;
  CHAR16 file_name[];
} EFI_FILE_INFO;

static const EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID = {
  0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

static const EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {
  0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

static const EFI_GUID EFI_FILE_INFO_GUID = {
  0x09576e92, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
