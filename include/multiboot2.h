#ifndef NEO_MULTIBOOT2_H
#define NEO_MULTIBOOT2_H

#include <nitekernel.h>

#define MB2_BOOTLOADER_MAGIC 0x36D76289

#define MB2_TAG_END        0
#define MB2_TAG_CMDLINE    1
#define MB2_TAG_BOOTLOADER 2
#define MB2_TAG_MODULE     3
#define MB2_TAG_BASIC_MEM  4
#define MB2_TAG_BOOTDEV    5
#define MB2_TAG_MMAP       6
#define MB2_TAG_VBE        7
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_TAG_ELF_SECTIONS 9
#define MB2_TAG_APM        10
#define MB2_TAG_EFI32      11
#define MB2_TAG_EFI64      12
#define MB2_TAG_SMBIOS     13
#define MB2_TAG_ACPI_OLD   14
#define MB2_TAG_ACPI_NEW   15
#define MB2_TAG_NETWORK    16
#define MB2_TAG_EFI_MMAP   17
#define MB2_TAG_EFI_BS     18
#define MB2_TAG_EFI32_IH   19
#define MB2_TAG_EFI64_IH   20
#define MB2_TAG_LOAD_ADDR  21

struct mb2_tag {
    u32 type;
    u32 size;
} __attribute__((packed));

struct mb2_info {
    u32 total_size;
    u32 reserved;
    struct mb2_tag tags[];
} __attribute__((packed));

struct mb2_tag_string {
    u32 type;
    u32 size;
    char string[];
} __attribute__((packed));

struct mb2_tag_module {
    u32 type;
    u32 size;
    u32 mod_start;
    u32 mod_end;
    char cmdline[];
} __attribute__((packed));

struct mb2_tag_basic_meminfo {
    u32 type;
    u32 size;
    u32 mem_lower;
    u32 mem_upper;
} __attribute__((packed));

struct mb2_mmap_entry {
    u64 addr;
    u64 len;
    u32 type;
    u32 zero;
} __attribute__((packed));

struct mb2_tag_mmap {
    u32 type;
    u32 size;
    u32 entry_size;
    u32 entry_version;
    struct mb2_mmap_entry entries[];
} __attribute__((packed));

#define MB2_MEMORY_AVAILABLE 1
#define MB2_MEMORY_RESERVED  2
#define MB2_MEMORY_ACPI      3
#define MB2_MEMORY_NVS       4
#define MB2_MEMORY_BADRAM    5

struct mb2_tag_framebuffer {
    u32 type;
    u32 size;
    u64 framebuffer_addr;
    u32 framebuffer_pitch;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u8  framebuffer_bpp;
    u8  framebuffer_type;
    u8  reserved;
} __attribute__((packed));

#define MB2_FB_TYPE_INDEXED 0
#define MB2_FB_TYPE_RGB     1
#define MB2_FB_TYPE_TEXT    2

struct mb2_tag_acpi {
    u32 type;
    u32 size;
    u8 rsdp[];
} __attribute__((packed));

#endif
