/*
 * Copyright 2021, Technology Innovation Institute
 * Copyright 2026, Unikie
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * NVIDIA Tegra Combined UART (TCU) driver for elfloader.
 * TCU uses HSP (Hardware Synchronization Primitives) shared mailboxes.
 *
 * Works on Tegra194 (Xavier) and Tegra234 (Orin).
 */

#include <devices_gen.h>
#include <drivers/common.h>
#include <drivers/uart.h>

#include <elfloader_common.h>

/*
 * HSP shared mailbox register offset and flags.
 * The mailbox is a 32-bit register where:
 * - Bits 0-23: data (up to 3 bytes)
 * - Bits 24-25: number of bytes (1-3)
 * - Bit 31: FULL flag (set when writing, cleared by receiver)
 */
#define HSP_SM_SHRD_MBOX_FULL   (1U << 31)
#define TCU_MBOX_NUM_BYTES_1    (1U << 24)

static int tegra_tcu_putchar(struct elfloader_device *dev, unsigned int c)
{
    volatile uint32_t *mbox = (volatile uint32_t *)dev->region_bases[0];

    /* Wait for mailbox to be empty (FULL bit cleared) */
    while (*mbox & HSP_SM_SHRD_MBOX_FULL);

    /* Write single character with byte count = 1 and FULL flag */
    *mbox = (c & 0xff) | TCU_MBOX_NUM_BYTES_1 | HSP_SM_SHRD_MBOX_FULL;

    /* Wait for it to be consumed */
    while (*mbox & HSP_SM_SHRD_MBOX_FULL);

    return 0;
}

static int tegra_tcu_init(struct elfloader_device *dev, UNUSED void *match_data)
{
    uart_set_out(dev);
    return 0;
}

static const struct dtb_match_table tegra_tcu_matches[] = {
    { .compatible = "nvidia,tegra194-tcu" },
    { .compatible = "nvidia,tegra234-tcu" },
    { .compatible = NULL /* sentinel */ },
};

static const struct elfloader_uart_ops tegra_tcu_ops = {
    .putc = &tegra_tcu_putchar,
};

static const struct elfloader_driver tegra_tcu = {
    .match_table = tegra_tcu_matches,
    .type = DRIVER_UART,
    .init = &tegra_tcu_init,
    .ops = &tegra_tcu_ops,
};

ELFLOADER_DRIVER(tegra_tcu);
