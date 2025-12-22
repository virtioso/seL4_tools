/*
 * Copyright 2024, TII
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <elfloader.h>

/* Platform init before UEFI exit - nothing needed for Orin AGX.
 * Fan control is done after UEFI exit in orinagx_fan_init(). */
void platform_init(void)
{
}
