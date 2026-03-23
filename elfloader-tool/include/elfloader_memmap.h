/*
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

/*
 * UEFI memory map support for the elfloader.
 *
 * The elfloader captures the EFI memory map before ExitBootServices()
 * and writes the usable memory regions into the kernel's .boot.memmap
 * section (avail_p_regs[]). The kernel reads this array directly at
 * boot — no magic headers, no ABI changes.
 */

#pragma once

#include <types.h>

#define AVAIL_P_REGS_MAX 64
#define BOOT_MEMMAP_SECTION ".boot.memmap"

struct elfloader_mem_region {
    uint64_t start;
    uint64_t end;
};

unsigned int efi_get_mem_regions(struct elfloader_mem_region *out,
                                unsigned int max_regions);
