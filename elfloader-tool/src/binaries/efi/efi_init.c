/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <binaries/efi/efi.h>
#include <elfloader_common.h>
#include <printf.h>

void *__application_handle = NULL;             // current efi application handler
efi_system_table_t *__efi_system_table = NULL; // current efi system table

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

/* EFI memory type names for debug output */
static const char *efi_mem_type_name(uint32_t type)
{
    switch (type) {
    case EFI_RESERVED_TYPE:             return "Reserved";
    case EFI_LOADER_CODE:               return "LoaderCode";
    case EFI_LOADER_DATA:               return "LoaderData";
    case EFI_BOOT_SERVICES_CODE:        return "BootSvcCode";
    case EFI_BOOT_SERVICES_DATA:        return "BootSvcData";
    case EFI_RUNTIME_SERVICES_CODE:     return "RuntimeCode";
    case EFI_RUNTIME_SERVICES_DATA:     return "RuntimeData";
    case EFI_CONVENTIONAL_MEMORY:       return "Conventional";
    case EFI_UNUSABLE_MEMORY:           return "Unusable";
    case EFI_ACPI_RECLAIM_MEMORY:       return "ACPIReclaim";
    case EFI_ACPI_MEMORY_NVS:           return "ACPI_NVS";
    case EFI_MEMORY_MAPPED_IO:          return "MMIO";
    case EFI_MEMORY_MAPPED_IO_PORT_SPACE: return "MMIO_Port";
    case EFI_PAL_CODE:                  return "PAL";
    case EFI_PERSISTENT_MEMORY:         return "Persistent";
    default:                            return "Unknown";
    }
}

/* Print full EFI memory map */
static void efi_print_memory_map(efi_memory_desc_t *memory_map,
                                  unsigned long map_size,
                                  unsigned long desc_size)
{
    printf("\n=== EFI Memory Map ===\n");

    unsigned long num_entries = map_size / desc_size;
    uint8_t *ptr = (uint8_t *)memory_map;

    printf("Total entries: %lu\n\n", num_entries);

    for (unsigned long i = 0; i < num_entries; i++) {
        efi_memory_desc_t *desc = (efi_memory_desc_t *)ptr;
        uint64_t start = desc->phys_addr;
        uint64_t end = start + (desc->num_pages * EFI_PAGE_SIZE);

        printf("  [%02lu] 0x%09lx - 0x%09lx  %-12s  (%lu pages)\n",
               i,
               (unsigned long)start,
               (unsigned long)end,
               efi_mem_type_name(desc->type),
               (unsigned long)desc->num_pages);

        ptr += desc_size;
    }

    printf("\n=== End EFI Memory Map ===\n\n");
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
     * As the number of existing memeory segments are unknown,
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

    /* Print memory map before exiting boot services */
    efi_print_memory_map(memory_map, map_size, desc_size);

    status = bts->exit_boot_services(__application_handle, key);
    return status;
}
