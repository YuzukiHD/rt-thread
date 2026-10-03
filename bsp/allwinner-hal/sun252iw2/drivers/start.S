/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Startup of the Allwinner sun252iw2: machine mode, the loader has brought up the
 * PSRAM and the console clock.
 */
    .section .start, "ax"
    .globl _start
_start:
    /* interrupts off, FPU on (FS = initial), machine previous mode */
    csrci   mstatus, 0x8
    li      t0, 0x2000
    csrs    mstatus, t0
    csrw    fcsr, zero
    csrw    mie, zero
    csrw    mip, zero

    la      t0, trap_entry
    csrw    mtvec, t0

    /* T-Head ISA extensions (cache maintenance instructions) and hardware misaligned access */
    li      t0, 0x400000
    li      t1, 0x8000
    or      t0, t0, t1
    csrs    0x7c0, t0                   /* mxstatus */

    /* invalidate caches, branch history and target buffers, then switch the caches on */
    li      t0, 0x70013
    csrw    0x7c2, t0                   /* mcor */
    fence
    fence.i
    li      t0, 0x11ff
    csrw    0x7c1, t0                   /* mhcr */
    fence
    li      t0, 0x16e30c
    csrw    0x7c5, t0                   /* mhint: prefetch, AMR, loop acceleration */
    fence

    la      sp, __stack_end__

    /* clear bss */
    la      t0, __bss_start
    la      t1, __bss_end
2:  bgeu    t0, t1, 3f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       2b
3:
    /* machine interrupts: software, timer, external */
    li      t0, 0x888
    csrw    mie, t0

    call    rtthread_startup
1:
    j       1b
