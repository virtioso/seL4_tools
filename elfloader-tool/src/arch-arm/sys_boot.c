/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 * Copyright 2021, HENSOLDT Cyber
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>

#include <drivers.h>
#include <drivers/uart.h>
#include <printf.h>
#include <types.h>
#include <abort.h>
#include <strops.h>
#include <cpuid.h>

#include <binaries/efi/efi.h>
#include <binaries/elf/elf.h>
#include <cpio/cpio.h>
#include <elfloader.h>
#include <elfloader_memmap.h>
#include <fdt.h>

#ifdef CONFIG_ARCH_AARCH64
#include <mode/structures.h>
#endif

/* 0xd00dfeed in big endian */
#define DTB_MAGIC (0xedfe0dd0)

/* Maximum alignment we need to preserve when relocating (64K) */
#define MAX_ALIGN_BITS (14)

#ifdef CONFIG_IMAGE_EFI
ALIGN(BIT(PAGE_BITS)) VISIBLE
char core_stack_alloc[CONFIG_MAX_NUM_NODES][BIT(PAGE_BITS)];
#endif

struct image_info kernel_info;
struct image_info user_info;
void const *dtb;
size_t dtb_size;
void const *kernel_elf_blob;

#ifdef CONFIG_IMAGE_EFI
static struct elfloader_mem_region efi_mem_regions[AVAIL_P_REGS_MAX];
#endif

extern void finish_relocation(int offset, void *_dynamic, unsigned int total_offset);
extern void flush_dcache_range(uintptr_t start, uintptr_t end);
void continue_boot(int was_relocated);

#ifdef CONFIG_ARCH_AARCH64
#define BLOCK_NORMAL  (0x1 | (4 << 2) | (3 << 8) | (1 << 10)) /* MT_NORMAL, ISH, AF */
#define BLOCK_DEVICE  (0x1 | (1 << 10) | (1ULL << 53) | (1ULL << 54)) /* MT_DEVICE_nGnRnE, AF, PXN, XN */

#define TABLE_DESC    0x3
#define GB_SHIFT      30
#define GB_SIZE       (1ULL << GB_SHIFT)
#define PUD_ENTRIES   512
#define KEEP_HEADERS_SIZE BIT(PAGE_BITS)
#endif

#if defined(CONFIG_ARCH_AARCH64) && defined(CONFIG_IMAGE_EFI)
extern void quiesce_hyp_mmu(void);
#endif

/*
 * Make sure the ELF loader is below the kernel's first virtual address
 * so that when we enable the MMU we can keep executing.
 */
extern char _DYNAMIC[];
void relocate_below_kernel(void)
{
    /*
     * These are the ELF loader's physical addresses,
     * since we are either running with MMU off or
     * identity-mapped.
     */
    uintptr_t UNUSED start = (uintptr_t)_text;
    uintptr_t end = (uintptr_t)_end;

    if (end <= kernel_info.virt_region_start) {
        /*
         * If the ELF loader is already below the kernel,
         * skip relocation.
         */
        continue_boot(0);
        return;
    }

#ifdef CONFIG_IMAGE_EFI
    /*
     * Note: we make the (potentially incorrect) assumption
     * that there is enough physical RAM below the kernel's first vaddr
     * to fit the ELF loader.
     * FIXME: do we need to make sure we don't accidentally wipe out the DTB too?
     */
    uintptr_t size = end - start;

    /*
     * we ROUND_UP size in this calculation so that all aligned things
     * (interrupt vectors, stack, etc.) end up in similarly aligned locations.
     * The strictes alignment requirement we have is the 64K-aligned AArch32
     * page tables, so we use that to calculate the new base of the elfloader.
     */
    uintptr_t new_base = kernel_info.virt_region_start - (ROUND_UP(size, MAX_ALIGN_BITS));
    uint32_t offset = start - new_base;
    printf("relocating from %p-%p to %p-%p... size=0x%x (padded size = 0x%x)\n", start, end, new_base, new_base + size,
           size, ROUND_UP(size, MAX_ALIGN_BITS));

    memmove((void *)new_base, (void *)start, size);

    /* call into assembly to do the finishing touches */
    finish_relocation(offset, _DYNAMIC, new_base);
#else
    printf("ERROR: The ELF loader does not support relocating itself. You"
           " probably need to move the kernel window higher, or the load"
           " address lower.\n");
    abort();
#endif
}

#ifdef CONFIG_ARCH_AARCH64
static void clear_hyp_boot_tables(void)
{
    for (int i = 0; i < BIT(PGD_BITS); i++) {
        _boot_pgd_up[i] = 0;
        _boot_pgd_down[i] = 0;
    }
    for (int i = 0; i < BIT(PUD_BITS); i++) {
        _boot_pud_up[i] = 0;
        _boot_pud_down[i] = 0;
    }
    for (int i = 0; i < BIT(PMD_BITS); i++) {
        _boot_pmd_up[i] = 0;
        _boot_pmd_down[i] = 0;
    }
}

static void map_1gb_range(uint64_t *pud, uint64_t start, uint64_t end, uint64_t attrs)
{
    uint64_t block_pa = start & ~(GB_SIZE - 1);
    uint64_t block_end = (end + GB_SIZE - 1) & ~(GB_SIZE - 1);

    for (; block_pa < block_end; block_pa += GB_SIZE) {
        unsigned int idx = (unsigned int)(block_pa >> GB_SHIFT);
        if (idx < PUD_ENTRIES) {
            if (attrs == BLOCK_NORMAL || pud[idx] == 0) {
                pud[idx] = block_pa | attrs;
            }
        }
    }
}

static int build_kernel_boot_info(struct image_info *kernel_boot_info)
{
    void const *cpio = _archive_start;
    size_t cpio_len = _archive_start_end - _archive_start;
    unsigned long cpio_file_size = 0;
    void const *elf_blob = cpio_get_file(cpio, cpio_len, "kernel.elf", &cpio_file_size);
    uint64_t kernel_phys_start, kernel_phys_end;
    uint64_t kernel_virt_start, kernel_virt_end;

    UNUSED_VARIABLE(cpio_file_size);

    if (elf_blob == NULL) {
        printf("ERROR: No kernel image present in archive\n");
        return -1;
    }
    if (elf_checkFile(elf_blob) != 0) {
        printf("ERROR: Kernel image not a valid ELF file\n");
        return -1;
    }
    if (elf_getMemoryBounds(elf_blob, 1, &kernel_phys_start, &kernel_phys_end) != 1) {
        printf("ERROR: Could not get kernel physical memory bounds\n");
        return -1;
    }
    if (elf_getMemoryBounds(elf_blob, 0, &kernel_virt_start, &kernel_virt_end) != 1) {
        printf("ERROR: Could not get kernel virtual memory bounds\n");
        return -1;
    }

    kernel_boot_info->phys_region_start = (paddr_t)kernel_phys_start;
    kernel_boot_info->phys_region_end = (paddr_t)ROUND_UP(kernel_phys_end, PAGE_BITS);
    kernel_boot_info->virt_region_start = (vaddr_t)kernel_virt_start;
    kernel_boot_info->virt_region_end = (vaddr_t)ROUND_UP(kernel_virt_end, PAGE_BITS);
    kernel_boot_info->virt_entry = (vaddr_t)elf_getEntryPoint(elf_blob);
    kernel_boot_info->phys_virt_offset = kernel_boot_info->phys_region_start - kernel_boot_info->virt_region_start;
    return 0;
}

static void map_user_image_destinations(void const *cpio, size_t cpio_len, paddr_t next_phys_addr)
{
    const char *elf_filename;
    unsigned int user_elf_offset = 2;

    cpio_get_entry(cpio, cpio_len, 0, &elf_filename, NULL);
    if (!elf_filename || strcmp(elf_filename, "kernel.elf") != 0) {
        printf("ERROR: Kernel image not first image in archive\n");
        abort();
    }

    cpio_get_entry(cpio, cpio_len, 1, &elf_filename, NULL);
    if (!elf_filename || strcmp(elf_filename, "kernel.dtb") != 0) {
        user_elf_offset = 1;
    }

    for (unsigned int i = 0; i < 1; i++) {
        unsigned long cpio_file_size = 0;
        void const *user_elf = cpio_get_entry(cpio, cpio_len, i + user_elf_offset,
                                              &elf_filename, &cpio_file_size);
        uint64_t min_vaddr, max_vaddr;
        size_t image_size;
        paddr_t mapped_end;

        UNUSED_VARIABLE(cpio_file_size);

        if (user_elf == NULL) {
            break;
        }
        if (elf_getMemoryBounds(user_elf, 0, &min_vaddr, &max_vaddr) != 1) {
            printf("ERROR: Could not get user image bounds\n");
            abort();
        }

        max_vaddr = ROUND_UP(max_vaddr, PAGE_BITS);
        image_size = (size_t)(max_vaddr - min_vaddr);
        mapped_end = ROUND_UP(next_phys_addr + image_size, PAGE_BITS) + KEEP_HEADERS_SIZE;
        map_1gb_range(_boot_pud_down, next_phys_addr, mapped_end, BLOCK_NORMAL);
        next_phys_addr = mapped_end;
    }
}

static void init_prebuilt_hyp_boot_vspace(struct image_info *kernel_boot_info, void const *dtb_addr)
{
    void const *cpio = _archive_start;
    size_t cpio_len = _archive_start_end - _archive_start;
    uintptr_t loader_start = (uintptr_t)_text;
    uintptr_t loader_end = (uintptr_t)_end;
    uintptr_t loader_size = loader_end - loader_start;
    paddr_t next_phys_addr = kernel_boot_info->phys_region_end;
    word_t pmd_index;

    clear_hyp_boot_tables();

    _boot_pgd_down[0] = (uint64_t)(uintptr_t)_boot_pud_down | TABLE_DESC;
    _boot_pgd_down[GET_PGD_INDEX(kernel_boot_info->virt_region_start)]
        = (uint64_t)(uintptr_t)_boot_pud_up | TABLE_DESC;
    _boot_pud_up[GET_PUD_INDEX(kernel_boot_info->virt_region_start)]
        = (uint64_t)(uintptr_t)_boot_pmd_up | TABLE_DESC;

    pmd_index = GET_PMD_INDEX(kernel_boot_info->virt_region_start);
    for (word_t i = pmd_index; i < BIT(PMD_BITS); i++) {
        _boot_pmd_up[i] = (((i - pmd_index) << ARM_2MB_BLOCK_BITS) + kernel_boot_info->phys_region_start)
                          | BIT(10)
#if CONFIG_MAX_NUM_NODES > 1
                          | (3 << 8)
#endif
                          | (4 << 2)
                          | BIT(0);
    }

    map_1gb_range(_boot_pud_down, loader_start, loader_end, BLOCK_NORMAL);
    map_1gb_range(_boot_pud_down,
                  kernel_boot_info->phys_region_start,
                  kernel_boot_info->phys_region_end,
                  BLOCK_NORMAL);

    if (loader_end > kernel_boot_info->virt_region_start) {
        uintptr_t relocated_start = kernel_boot_info->virt_region_start - ROUND_UP(loader_size, MAX_ALIGN_BITS);
        map_1gb_range(_boot_pud_down, relocated_start, relocated_start + loader_size, BLOCK_NORMAL);
    }

    if (dtb_addr) {
        size_t dtb_bytes = fdt_size(dtb_addr);
        if (dtb_bytes == 0) {
            printf("ERROR: Invalid device tree blob supplied\n");
            abort();
        }

        map_1gb_range(_boot_pud_down, (uint64_t)(uintptr_t)dtb_addr,
                      (uint64_t)(uintptr_t)dtb_addr + dtb_bytes, BLOCK_NORMAL);

        next_phys_addr = ROUND_UP(next_phys_addr, PAGE_BITS);
        map_1gb_range(_boot_pud_down, next_phys_addr, next_phys_addr + dtb_bytes, BLOCK_NORMAL);
        next_phys_addr = ROUND_UP(next_phys_addr + dtb_bytes, PAGE_BITS);
    } else {
        next_phys_addr = ROUND_UP(next_phys_addr, PAGE_BITS);
    }

    map_user_image_destinations(cpio, cpio_len, next_phys_addr);

    volatile void *uart_mmio = uart_get_mmio();
    if (uart_mmio) {
        uint64_t uart_pa = (uint64_t)(uintptr_t)uart_mmio;
        map_1gb_range(_boot_pud_down, uart_pa, uart_pa + 1, BLOCK_DEVICE);
    }
}
#endif /* CONFIG_ARCH_AARCH64 */

/*
 * Entry point.
 *
 * Unpack images, setup the MMU, jump to the kernel.
 */
void main(UNUSED void *arg)
{
    void *bootloader_dtb = NULL;

    /* initialize platform to a state where we can print to a UART */
    if (initialise_devices()) {
        printf("ERROR: Did not successfully return from initialise_devices()\n");
        abort();
    }

    platform_init();

    /* Print welcome message. */
    printf("\nELF-loader started on ");
    print_cpuid();
    printf("  paddr=[%p..%p]\n", _text, (uintptr_t)_end - 1);

#if defined(CONFIG_IMAGE_UIMAGE)

    /* U-Boot passes a DTB. Ancient bootloaders may pass atags. When booting via
     * bootelf argc is NULL.
     */
    if (arg && (DTB_MAGIC == *(uint32_t *)arg)) {
        bootloader_dtb = arg;
    }

#elif defined(CONFIG_IMAGE_EFI)

    if (efi_exit_boot_services() != EFI_SUCCESS) {
        printf("ERROR: Unable to exit UEFI boot services!\n");
        abort();
    }

    bootloader_dtb = efi_get_fdt();

#endif

#ifdef CONFIG_ARCH_AARCH64
    if (is_hyp_mode()) {
        struct image_info kernel_boot_info;

        if (build_kernel_boot_info(&kernel_boot_info) != 0) {
            abort();
        }
#ifdef CONFIG_IMAGE_EFI
        quiesce_hyp_mmu();
#endif
        init_prebuilt_hyp_boot_vspace(&kernel_boot_info, bootloader_dtb);
        arm_enable_hyp_mmu(_boot_pgd_down);
    }
#endif

    if (bootloader_dtb) {
        printf("  dtb=%p\n", bootloader_dtb);
    } else {
        printf("No DTB passed in from boot loader.\n");
    }

    /* Unpack ELF images into memory. */
    unsigned int num_apps = 0;
    int ret = load_images(&kernel_info, &user_info, 1, &num_apps,
                          bootloader_dtb, &dtb, &dtb_size);
    if (0 != ret) {
        printf("ERROR: image loading failed\n");
        abort();
    }

    if (num_apps != 1) {
        printf("ERROR: expected to load just 1 app, actually loaded %u apps\n",
               num_apps);
        abort();
    }
    /*
     * We don't really know where we've been loaded.
     * It's possible that EFI loaded us in a place
     * that will become part of the 'kernel window'
     * once we switch to the boot page tables.
     * Make sure this is not the case.
     */
    relocate_below_kernel();
    printf("ERROR: Relocation failed, aborting!\n");
    abort();
}

void continue_boot(int was_relocated)
{
    if (was_relocated) {
        printf("ELF loader relocated, continuing boot...\n");
    }

    /*
     * If we were relocated, we need to re-initialise the
     * driver model so all its pointers are set up properly.
     */
    if (was_relocated) {
        if (initialise_devices()) {
            printf("ERROR: Did not successfully return from initialise_devices()\n");
            abort();
        }
    }

#if (defined(CONFIG_ARCH_ARM_V7A) || defined(CONFIG_ARCH_ARM_V8A)) && !defined(CONFIG_ARM_HYPERVISOR_SUPPORT)
    if (is_hyp_mode()) {
        extern void leave_hyp(void);
        leave_hyp();
    }
#endif

    /*
     * Write EFI memory regions into the kernel's .boot.memmap section.
     * Find the section by name in the kernel ELF, compute its physical
     * address in the loaded image, and overwrite the default regions.
     */
#ifdef CONFIG_IMAGE_EFI
    if (kernel_elf_blob) {
        unsigned int num_regions = efi_get_mem_regions(
            efi_mem_regions, AVAIL_P_REGS_MAX);

        if (num_regions > 0) {
            /* Find the .boot.memmap section in the kernel ELF */
            int sec_idx = -1;
            unsigned int num_secs = elf_getNumSections(kernel_elf_blob);
            for (unsigned int s = 0; s < num_secs; s++) {
                char const *name = elf_getSectionName(kernel_elf_blob, s);
                if (name && strcmp(name, BOOT_MEMMAP_SECTION) == 0) {
                    sec_idx = s;
                    break;
                }
            }
            if (sec_idx >= 0) {
                uint64_t section_vaddr = elf_getSectionAddr(kernel_elf_blob, sec_idx);
                paddr_t section_paddr = kernel_info.phys_region_start +
                    (section_vaddr - kernel_info.virt_region_start);

                /* Each entry is {uint64_t start, uint64_t end} — same layout
                 * as kernel's p_region_t on aarch64. */
                struct elfloader_mem_region *dest =
                    (struct elfloader_mem_region *)section_paddr;

                for (unsigned int i = 0; i < num_regions; i++) {
                    dest[i] = efi_mem_regions[i];
                }
                /* Zero out remaining entries */
                for (unsigned int i = num_regions; i < AVAIL_P_REGS_MAX; i++) {
                    dest[i].start = 0;
                    dest[i].end = 0;
                }

                printf("EFI memmap: %u regions written to kernel " BOOT_MEMMAP_SECTION
                       " at phys 0x%lx\n", num_regions, (unsigned long)section_paddr);
                for (unsigned int i = 0; i < num_regions; i++) {
                    printf("  mem[%u]: 0x%lx - 0x%lx\n", i,
                           (unsigned long)efi_mem_regions[i].start,
                           (unsigned long)(efi_mem_regions[i].end - 1));
                }
            } else {
                printf("WARNING: kernel has no " BOOT_MEMMAP_SECTION
                       " section, using DTS default\n");
            }
        } else {
            printf("WARNING: No EFI memory regions found, using DTS default\n");
        }
    }
#endif

#ifdef CONFIG_ARCH_AARCH64
    flush_dcache_range(kernel_info.phys_region_start, kernel_info.phys_region_end);
    flush_dcache_range(user_info.phys_region_start, user_info.phys_region_end);
    if (dtb && dtb_size) {
        flush_dcache_range((uintptr_t)dtb, (uintptr_t)dtb + dtb_size);
    }
#endif

    /* Setup MMU. */
    if (is_hyp_mode()) {
#ifndef CONFIG_ARCH_AARCH64
        init_hyp_boot_vspace(&kernel_info);
#endif
    } else {
        /* If we are not in HYP mode, we enable the SV MMU and paging
         * just in case the kernel does not support hyp mode. */
        init_boot_vspace(&kernel_info);
    }

#if CONFIG_MAX_NUM_NODES > 1
    smp_boot();
#endif /* CONFIG_MAX_NUM_NODES */

    if (is_hyp_mode()) {
        printf("Enabling hypervisor MMU and jumping to entry point...\n\n");
        arm_enable_hyp_mmu(_boot_pgd_down);
    } else {
        printf("Enabling MMU and jumping to entry point...\n\n");
        arm_enable_mmu();
    }

    /* Enter kernel. The UART is no longer accessible here. */
    ((init_arm_kernel_t)kernel_info.virt_entry)(user_info.phys_region_start,
                                                user_info.phys_region_end,
                                                user_info.phys_virt_offset,
                                                user_info.virt_entry,
                                                (word_t)dtb,
                                                dtb_size);

    /* We should never get here. */
    abort();
}
