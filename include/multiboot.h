#ifndef MULTIBOOT_H
#define MULTIBOOT_H
#include "include/types.h"
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002
#define MULTIBOOT_FLAG_MEM         (1u << 0)   /* mem_lower / mem_upper valid   */
#define MULTIBOOT_FLAG_MMAP        (1u << 6)   /* mmap_length / mmap_addr valid */

/* Multiboot 0.6.96 information structure (fields up to the memory map). */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;      /* KiB below 1 MiB */
    uint32_t mem_upper;      /* KiB above 1 MiB */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;    /* bytes in the memory map */
    uint32_t mmap_addr;      /* physical address of the first entry */
} multiboot_info_t;

/* One memory-map entry. `size` does NOT include the 4 bytes of `size` itself. */
typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;           /* 1 = available RAM, everything else is not usable */
} __attribute__((packed)) multiboot_mmap_entry_t;

#define MULTIBOOT_MMAP_AVAILABLE 1
#endif
