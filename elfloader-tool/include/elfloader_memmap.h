/*
 * Copyright 2026, Technology Innovation Institute
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * Shared header between elfloader and seL4 kernel for passing the
 * physical memory map from the elfloader (e.g. from the UEFI memory
 * map) to the kernel at boot.
 *
 * The elfloader places this structure in a static buffer and passes
 * its address as the dtb_addr parameter to the kernel. The kernel
 * detects the header by checking for the magic value, extracts the
 * memory regions, and finds the real DTB at dtb_paddr.
 *
 * Layout in memory:
 *
 *   dtb_addr -->  +---------------------------+
 *                 | elfloader_bootinfo_header  |
 *                 |   .magic                   |
 *                 |   .num_mem_regions         |
 *                 |   .dtb_paddr               |
 *                 |   .dtb_size                |
 *                 +---------------------------+
 *                 | elfloader_mem_region[0]    |
 *                 | elfloader_mem_region[1]    |
 *                 | ...                        |
 *                 | elfloader_mem_region[N-1]  |
 *                 +---------------------------+
 *
 *   dtb_paddr --> +---------------------------+
 *                 | FDT blob (original loc)    |
 *                 +---------------------------+
 */

#pragma once

#include <types.h>

#define ELFLOADER_BOOTINFO_MAGIC 0x4542494e  /* "EBIN" */

#define ELFLOADER_MAX_MEM_REGIONS 64

struct elfloader_mem_region {
    uint64_t start;
    uint64_t end;
};

struct elfloader_bootinfo_header {
    uint32_t magic;             /* ELFLOADER_BOOTINFO_MAGIC */
    uint32_t num_mem_regions;   /* number of elfloader_mem_region entries */
    uint64_t dtb_paddr;         /* physical address of the real FDT blob */
    uint32_t dtb_size;          /* size of FDT blob in bytes */
    uint32_t reserved;          /* padding for alignment */
};
