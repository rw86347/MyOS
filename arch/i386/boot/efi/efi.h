/*
 * efi.h — the UEFI types the loader needs, and nothing else.
 *
 * The firmware talks the Microsoft x64 calling convention (first four
 * arguments in RCX, RDX, R8, R9). clang -target x86_64-unknown-windows
 * does that. Pointers are 8 bytes; structs use natural alignment, not
 * packed, so the tables match the spec's x64 layout.
 */
#ifndef MYOS_EFI_H
#define MYOS_EFI_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef u64                efi_status;
typedef void              *efi_handle;
typedef u64                efi_phys;

#define EFI_SUCCESS              0ull
#define EFI_BUFFER_TOO_SMALL     0x8000000000000005ull
#define EFI_NOT_FOUND            0x800000000000000eull

#define EFI_PAGE 4096ull

enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
};

enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
};

#define EFI_FILE_MODE_READ 1ull
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL 2u

#define PixelRedGreenBlueReserved8BitPerColor 0
#define PixelBlueGreenRedReserved8BitPerColor 1
#define PixelBitMask                          2

typedef struct {
    u32 data1;
    u16 data2;
    u16 data3;
    u8  data4[8];
} efi_guid;

typedef struct {
    u64 signature;
    u32 revision;
    u32 header_size;
    u32 crc32;
    u32 reserved;
} efi_table_header;

typedef struct {
    u32 type;
    u32 pad;
    u64 phys;
    u64 virt;
    u64 pages;
    u64 attr;
} efi_memory_desc;

struct efi_text_out {
    void *reset;
    efi_status (*output_string)(struct efi_text_out *this, u16 *string);
};

/* Boot services, in spec order, so the function we call sits at the
   offset the firmware put it. Unused slots are void * so the layout
   stays honest. */
struct efi_boot_services {
    efi_table_header hdr;
    void *raise_tpl;
    void *restore_tpl;
    efi_status (*allocate_pages)(u32 type, u32 memtype, u64 pages, efi_phys *mem);
    efi_status (*free_pages)(efi_phys mem, u64 pages);
    efi_status (*get_memory_map)(u64 *size, efi_memory_desc *map, u64 *key,
                                 u64 *desc_size, u32 *desc_ver);
    efi_status (*allocate_pool)(u32 memtype, u64 size, void **buf);
    efi_status (*free_pool)(void *buf);
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    efi_status (*handle_protocol)(efi_handle handle, efi_guid *protocol, void **iface);
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    efi_status (*exit_boot_services)(efi_handle image, u64 map_key);
    void *get_next_monotonic_count;
    efi_status (*stall)(u64 microseconds);
    efi_status (*set_watchdog_timer)(u64 seconds, u64 code, u64 data_size, u16 *data);
    void *connect_controller;
    void *disconnect_controller;
    efi_status (*open_protocol)(efi_handle handle, efi_guid *protocol, void **iface,
                                efi_handle agent, efi_handle controller, u32 attr);
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    void *locate_handle_buffer;
    efi_status (*locate_protocol)(efi_guid *protocol, void *reg, void **iface);
};

struct efi_system_table {
    efi_table_header hdr;
    u16 *firmware_vendor;
    u32 firmware_revision;
    u32 _pad;
    efi_handle con_in_handle;
    void *con_in;
    efi_handle con_out_handle;
    struct efi_text_out *con_out;
    efi_handle stderr_handle;
    struct efi_text_out *std_err;
    void *runtime_services;
    struct efi_boot_services *boot;
    u64 ntables;
    void *config_table;
};

struct efi_loaded_image {
    u32 revision;
    u32 _pad;
    efi_handle parent;
    struct efi_system_table *st;
    efi_handle device;
};

struct efi_file {
    u64 revision;
    efi_status (*open)(struct efi_file *this, struct efi_file **newh,
                       u16 *name, u64 mode, u64 attr);
    efi_status (*close)(struct efi_file *this);
    void *delete;
    efi_status (*read)(struct efi_file *this, u64 *size, void *buf);
    void *write;
    efi_status (*get_position)(struct efi_file *this, u64 *pos);
    efi_status (*set_position)(struct efi_file *this, u64 pos);
};

struct efi_simple_fs {
    u64 revision;
    efi_status (*open_volume)(struct efi_simple_fs *this, struct efi_file **root);
};

struct efi_gop_mode_info {
    u32 version;
    u32 width;
    u32 height;
    u32 pixel_format;
    u32 red_mask, green_mask, blue_mask, reserved_mask;
    u32 pixels_per_scan_line;
};

struct efi_gop_mode {
    u32 max_mode;
    u32 mode;
    struct efi_gop_mode_info *info;
    u64 info_size;
    efi_phys fb_base;
    u64 fb_size;
};

struct efi_gop {
    efi_status (*query_mode)(struct efi_gop *this, u32 mode, u64 *size,
                             struct efi_gop_mode_info **info);
    efi_status (*set_mode)(struct efi_gop *this, u32 mode);
    void *blt;
    struct efi_gop_mode *mode;
};

/* GUIDs from the UEFI spec, written out so we don't need a library. */
static const efi_guid loaded_image_guid = {
    0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};
static const efi_guid simple_fs_guid = {
    0x964E5B22, 0x6459, 0x11d2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B }
};
static const efi_guid gop_guid = {
    0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a }
};

#endif
