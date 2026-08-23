#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_FLAG_MODS        0x00000008

// Only the fields we actually use are named; the rest are kept as
// padding so the layout still matches the real multiboot1 info struct.
typedef struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
} __attribute__((packed)) multiboot_info_t;

// One entry per "module /boot/Storefile" line in the bootloader configuration file.
typedef struct multiboot_module {
    uint32_t mod_start; // physical address the bootloader placed the module at
    uint32_t mod_end;   // one past the last byte
    uint32_t string;    // pointer to the module's command line (its path)
    uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

#endif
