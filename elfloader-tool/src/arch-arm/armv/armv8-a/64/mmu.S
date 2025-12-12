/*
 * Copyright 2020, Data61, CSIRO (ABN 41 687 119 230)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <autoconf.h>
#include <elfloader/gen_config.h>
#include <assembler.h>
#include <mode/assembler.h>

#if CONFIG_MAX_NUM_NODES > 1
#define TCR_ISH     TCR_SHARED
#else
#define TCR_ISH     0
#endif

.text

.extern _boot_pgd_up
.extern _boot_pgd_down
.extern arm_vector_table

BEGIN_FUNC(invalidate_icache)
    ic      iallu
    dsb     nsh
    isb
    ret
END_FUNC(invalidate_icache)

.extern _text
.extern _end
.extern _boot_pgd_down
.extern _boot_pud_down
.extern _boot_pgd_up
.extern _boot_pud_up
.extern _boot_pmd_up

BEGIN_FUNC(flush_dcache)
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    adrp    x0, _text
    adrp    x1, _end
    add     x1, x1, #4096
    bl      flush_dcache_range

    adrp    x0, _boot_pgd_down
    add     x1, x0, #4096
    bl      flush_dcache_range

    adrp    x0, _boot_pud_down
    add     x1, x0, #4096
    bl      flush_dcache_range

    adrp    x0, _boot_pgd_up
    add     x1, x0, #4096
    bl      flush_dcache_range

    adrp    x0, _boot_pud_up
    add     x1, x0, #4096
    bl      flush_dcache_range

    adrp    x0, _boot_pmd_up
    add     x1, x0, #4096
    bl      flush_dcache_range

    ldp     x29, x30, [sp], #16
    ret
END_FUNC(flush_dcache)

/* x0 = start address, x1 = end address */
BEGIN_FUNC(flush_dcache_range)
    mrs     x2, ctr_el0
    ubfx    x2, x2, #16, #4
    mov     x3, #4
    lsl     x3, x3, x2

1:  dc      civac, x0
    add     x0, x0, x3
    cmp     x0, x1
    b.lo    1b

    dsb     sy
    isb
    ret
END_FUNC(flush_dcache_range)

BEGIN_FUNC(arm_enable_mmu)
    /* We call nested functions, follow the ABI. */
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp

    bl      flush_dcache

    /* Ensure I-cache, D-cache and mmu are disabled for EL1/Stage1 */
    disable_mmu sctlr_el1 , x8

    /*
     * Invalidate the local I-cache so that any instructions fetched
     * speculatively are discarded.
     */
    bl      invalidate_icache

    /*
     *   DEVICE_nGnRnE      000     00000000
     *   DEVICE_nGnRE       001     00000100
     *   DEVICE_GRE         010     00001100
     *   NORMAL_NC          011     01000100
     *   NORMAL             100     11111111
     *   NORMAL_WT          101     10101010
     */
    ldr     x5, =MAIR(0x00, MT_DEVICE_nGnRnE) | \
                 MAIR(0x04, MT_DEVICE_nGnRE) | \
                 MAIR(0x0c, MT_DEVICE_GRE) | \
                 MAIR(0x44, MT_NORMAL_NC) | \
                 MAIR(0xff, MT_NORMAL) | \
                 MAIR(0xaa, MT_NORMAL_WT)
    msr     mair_el1, x5

    ldr     x10, =TCR_TxSZ(48) | TCR_IRGN_WBWA | TCR_ORGN_WBWA | TCR_TG0_4K | TCR_TG1_4K | TCR_ASID16 | TCR_ISH
    mrs     x9, ID_AA64MMFR0_EL1
    bfi     x10, x9, #32, #3
    msr     tcr_el1, x10

    /* Setup page tables */
    adrp    x8, _boot_pgd_down
    msr     ttbr0_el1, x8
    adrp    x8, _boot_pgd_up
    msr     ttbr1_el1, x8
    isb

    /* invalidate all TLB entries for EL1 */
    tlbi    vmalle1is
    dsb     ish
    isb

    enable_mmu sctlr_el1 , x8

    adrp    x8, arm_vector_table
    msr     vbar_el1, x8

    ldp     x29, x30, [sp], #16
    ret
END_FUNC(arm_enable_mmu)
