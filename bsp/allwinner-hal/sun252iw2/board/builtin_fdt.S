/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The device tree blob of the board, built from dts/ and linked into the image.
 */
    .section .rodata.dtb, "a"
    .balign 8
    .globl __dtb_start
__dtb_start:
    .incbin RT_BUILTIN_FDT_PATH
    .globl __dtb_end
__dtb_end:
