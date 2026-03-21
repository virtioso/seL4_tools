/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2026, Technology Innovation Institute
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <binaries/efi/efi.h>
#include <elfloader_common.h>
#include <elfloader_memmap.h>
#include <printf.h>

void *__application_handle = NULL;             // current efi application handler
efi_system_table_t *__efi_system_table = NULL; // current efi system table

/*
 * EFI memory map captured before ExitBootServices().
 * After ExitBootServices(), the EFI pool allocator is gone, so we must
 * save the map into a static buffer.
 */
static efi_memory_desc_t efi_mmap_buf[512];
static unsigned long efi_mmap_num_entries;
static unsigned long efi_mmap_desc_size;

extern void _start(void);
unsigned int efi_main(uintptr_t application_handle, uintptr_t efi_system_table)
{
    clear_bss();
    __application_handle = (void *)application_handle;
    __efi_system_table = (efi_system_table_t *)efi_system_table;
    _start();
    return 0;
}

void *efi_get_fdt(void)
{
    efi_guid_t fdt_guid = make_efi_guid(0xb1b621d5, 0xf19c, 0x41a5,  0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0);
    efi_config_table_t *tables = (efi_config_table_t *)__efi_system_table->tables;

    for (uint32_t i = 0; i < __efi_system_table->nr_tables; i++) {
        if (!efi_guideq(fdt_guid, tables[i].guid))
            continue;

        return (void *)tables[i].table;
    }

    return NULL;
}

/* Before starting the kernel we should notify the UEFI firmware about it
 * otherwise the internal watchdog may reboot us after 5 min.
 *
 * This means boot time services are not available anymore. We should store
 * system information e.g. current memory map and pass them to kernel.
 */
unsigned long efi_exit_boot_services(void)
{
    unsigned long status;
    efi_memory_desc_t *memory_map;
    unsigned long map_size;
    unsigned long desc_size, key;
    uint32_t desc_version;

    efi_boot_services_t *bts = get_efi_boot_services();

    /*
     * As the number of existing memory segments are unknown,
     * we need to resort to a trial and error to guess that.
     * We start from 32 and increase it by one until get a valid value.
     */
    map_size = sizeof(*memory_map) * 32;

again:
    status = bts->allocate_pool(EFI_LOADER_DATA, map_size, (void **)&memory_map);

    if (status != EFI_SUCCESS)
        return status;

    status = bts->get_memory_map(&map_size, memory_map, &key, &desc_size, &desc_version);
    if (status == EFI_BUFFER_TOO_SMALL) {
        bts->free_pool(memory_map);

        map_size += sizeof(*memory_map);
        goto again;
    }

    if (status != EFI_SUCCESS){
        bts->free_pool(memory_map);
        return status;
    }

    /*
     * Save the memory map before ExitBootServices() invalidates the
     * pool allocation. We copy descriptor-by-descriptor because
     * desc_size may differ from sizeof(efi_memory_desc_t).
     */
    efi_mmap_desc_size = desc_size;
    efi_mmap_num_entries = map_size / desc_size;
    if (efi_mmap_num_entries > ARRAY_SIZE(efi_mmap_buf)) {
        efi_mmap_num_entries = ARRAY_SIZE(efi_mmap_buf);
    }
    for (unsigned long i = 0; i < efi_mmap_num_entries; i++) {
        efi_memory_desc_t *src = (efi_memory_desc_t *)((uint8_t *)memory_map + i * desc_size);
        efi_mmap_buf[i] = *src;
    }

    status = bts->exit_boot_services(__application_handle, key);
    return status;
}

/*
 * Build the physical memory region list from the saved EFI memory map.
 * Filters for EfiConventionalMemory and EfiBootServicesCode/Data
 * (reclaimable after ExitBootServices), sorts by address, and merges
 * adjacent regions.
 *
 * Returns the number of regions written to out[].
 */
unsigned int efi_get_mem_regions(struct elfloader_mem_region *out,
                                unsigned int max_regions)
{
    unsigned int count = 0;

    /* Collect usable memory regions */
    for (unsigned long i = 0; i < efi_mmap_num_entries && count < max_regions; i++) {
        efi_memory_desc_t *md = &efi_mmap_buf[i];
        if (md->type != EFI_CONVENTIONAL_MEMORY &&
            md->type != EFI_BOOT_SERVICES_CODE &&
            md->type != EFI_BOOT_SERVICES_DATA) {
            continue;
        }

        /* Align to 2 MiB: round start up, end down. This ensures every
         * reported region can be mapped with large pages without including
         * any firmware carveout bytes. */
        uint64_t start = md->phys_addr;
        uint64_t end = start + md->num_pages * EFI_PAGE_SIZE;
        start = (start + 0x1FFFFF) & ~0x1FFFFFULL;
        end = end & ~0x1FFFFFULL;

        if (start >= end) {
            continue;  /* too small after alignment */
        }

        out[count].start = start;
        out[count].end = end;
        count++;
    }

    /* Insertion sort by start address */
    for (unsigned int i = 1; i < count; i++) {
        struct elfloader_mem_region tmp = out[i];
        unsigned int j = i;
        while (j > 0 && out[j - 1].start > tmp.start) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = tmp;
    }

    /* Merge adjacent/overlapping regions */
    unsigned int merged = 0;
    for (unsigned int i = 0; i < count; i++) {
        if (merged > 0 && out[i].start <= out[merged - 1].end) {
            if (out[i].end > out[merged - 1].end) {
                out[merged - 1].end = out[i].end;
            }
        } else {
            out[merged] = out[i];
            merged++;
        }
    }

    return merged;
}
