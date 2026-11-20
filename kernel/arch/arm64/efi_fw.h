/*
 * UEFI firmware interfaces used by the loader stub (UEFI specification
 * 2.x): just the tables, protocols and fields Sonar calls. Shared by
 * efi.c and efi_update.c.
 */
#pragma once
#include "types.h"

typedef u64 efi_status;
typedef void *efi_handle;
#define EFI_ERROR(s) ((s) >> 63)
#define EFI_BUFFER_TOO_SMALL (0x8000000000000000ULL | 5)
#define EFI_LOAD_ERROR (0x8000000000000000ULL | 1)

enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
    EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
};
#define EFI_MEMORY_WB 0x8ULL
#define AllocateAnyPages 0
#define EFI_FILE_MODE_READ 1ULL

typedef struct {
    u32 a;
    u16 b, c;
    u8 d[8];
} efi_guid;

static const efi_guid DTB_GUID = { 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } };
static const efi_guid ACPI20_GUID = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
static const efi_guid GOP_GUID = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static const efi_guid LOADED_IMAGE_GUID = { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const efi_guid SIMPLE_FS_GUID = { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

struct efi_table_header {
    u64 signature;
    u32 revision, header_size, crc32, reserved;
};

struct efi_text_out {
    efi_status (*reset)(struct efi_text_out *self, u8 extended);
    efi_status (*output_string)(struct efi_text_out *self, const u16 *s);
};

struct efi_memory_descriptor {
    u32 type, pad;
    u64 phys, virt, pages, attr;
};

struct efi_boot_services {
    struct efi_table_header hdr;
    void *raise_tpl, *restore_tpl;
    efi_status (*allocate_pages)(u32 type, u32 memory_type, u64 pages, u64 *addr);
    efi_status (*free_pages)(u64 addr, u64 pages);
    efi_status (*get_memory_map)(u64 *size, struct efi_memory_descriptor *map, u64 *key,
                                 u64 *desc_size, u32 *desc_version);
    efi_status (*allocate_pool)(u32 memory_type, u64 size, void **buf);
    efi_status (*free_pool)(void *buf);
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event, *check_event;
    void *install_protocol_interface, *reinstall_protocol_interface, *uninstall_protocol_interface;
    efi_status (*handle_protocol)(efi_handle handle, const efi_guid *guid, void **iface);
    void *reserved, *register_protocol_notify, *locate_handle, *locate_device_path;
    void *install_configuration_table;
    efi_status (*load_image)(bool boot_policy, efi_handle parent, void *device_path,
                             void *source, u64 source_size, efi_handle *image);
    efi_status (*start_image)(efi_handle image, u64 *exit_data_size, u16 **exit_data);
    void *exit;
    efi_status (*unload_image)(efi_handle image);
    efi_status (*exit_boot_services)(efi_handle image, u64 map_key);
    void *get_next_monotonic_count;
    efi_status (*stall)(u64 microseconds);
    void *set_watchdog_timer;
    void *connect_controller, *disconnect_controller;
    void *open_protocol, *close_protocol, *open_protocol_information;
    void *protocols_per_handle;
    efi_status (*locate_handle_buffer)(u32 search_type, const efi_guid *guid, void *key,
                                       u64 *count, efi_handle **handles);
    efi_status (*locate_protocol)(const efi_guid *guid, void *registration, void **iface);
};

struct efi_configuration_table {
    efi_guid guid;
    void *table;
};

struct efi_system_table {
    struct efi_table_header hdr;
    const u16 *firmware_vendor;
    u32 firmware_revision;
    efi_handle console_in_handle;
    void *con_in;
    efi_handle console_out_handle;
    struct efi_text_out *con_out;
    efi_handle standard_error_handle;
    void *std_err;
    struct efi_runtime_services *runtime_services;
    struct efi_boot_services *boot_services;
    u64 number_of_table_entries;
    struct efi_configuration_table *configuration_table;
};

struct efi_loaded_image {
    u32 revision;
    efi_handle parent_handle;
    struct efi_system_table *system_table;
    efi_handle device_handle;
    void *file_path;
    void *reserved;
    u32 load_options_size;
    void *load_options;
    void *image_base;
    u64 image_size;
    u32 image_code_type, image_data_type;
    void *unload;
};

struct efi_file {
    u64 revision;
    efi_status (*open)(struct efi_file *self, struct efi_file **out, const u16 *name, u64 mode,
                       u64 attributes);
    efi_status (*close)(struct efi_file *self);
    efi_status (*delete_)(struct efi_file *self);
    efi_status (*read)(struct efi_file *self, u64 *size, void *buf);
    efi_status (*write)(struct efi_file *self, u64 *size, const void *buf);
    efi_status (*get_position)(struct efi_file *self, u64 *pos);
    efi_status (*set_position)(struct efi_file *self, u64 pos);
    void *get_info, *set_info;
    efi_status (*flush)(struct efi_file *self);
};

struct efi_simple_fs {
    u64 revision;
    efi_status (*open_volume)(struct efi_simple_fs *self, struct efi_file **root);
};

struct efi_gop_mode_info {
    u32 version, width, height, pixel_format;
    u32 red_mask, green_mask, blue_mask, reserved_mask;
    u32 pixels_per_scan_line;
};

struct efi_gop_mode {
    u32 max_mode, mode;
    struct efi_gop_mode_info *info;
    u64 size_of_info;
    u64 frame_buffer_base, frame_buffer_size;
};

struct efi_gop {
    void *query_mode, *set_mode, *blt;
    struct efi_gop_mode *mode;
};

/* EFI_TIME and GetTime (UEFI spec 8.3); the runtime services table starts
 * with a header, then GetTime. Machines without an RTC device the kernel
 * knows (no PL031) get the wall clock from here. */
struct efi_time {
    u16 year;
    u8 month, day, hour, minute, second, pad1;
    u32 nanosecond;
    i16 timezone;
    u8 daylight, pad2;
};
struct efi_runtime_services {
    struct efi_table_header hdr;
    efi_status (*get_time)(struct efi_time *t, void *caps);
    void *set_time, *get_wakeup_time, *set_wakeup_time, *set_virtual_address_map;
    void *convert_pointer;
    efi_status (*get_variable)(const u16 *name, const efi_guid *vendor, u32 *attributes,
                               u64 *size, void *data);
    void *get_next_variable_name;
    efi_status (*set_variable)(const u16 *name, const efi_guid *vendor, u32 attributes,
                               u64 size, const void *data);
};

/* Block I/O protocol (13.9) */
struct efi_block_io_media {
    u32 media_id;
    u8 removable, present, logical_partition, read_only, write_caching;
    u32 block_size, io_align;
    u64 last_block;
};
struct efi_block_io {
    u64 revision;
    struct efi_block_io_media *media;
    efi_status (*reset)(struct efi_block_io *self, u8 extended);
    efi_status (*read_blocks)(struct efi_block_io *self, u32 media_id, u64 lba, u64 size,
                              void *buf);
};
#define EFI_BY_PROTOCOL 2
#define EFI_NOT_FOUND (0x8000000000000000ULL | 14)
#define EFI_VARIABLE_NON_VOLATILE 1
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 2
#define EFI_VARIABLE_RUNTIME_ACCESS 4

/* efi_update.c: boot a newer, signed kernel from the SonarFS system disk
 * (docs/updates.md). Returns only if this kernel should keep booting. */
void efi_boot_update(efi_handle image, struct efi_system_table *st, struct efi_loaded_image *li,
                     void (*say)(const char *));
