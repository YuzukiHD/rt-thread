/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Kernel tick of the C907: the CLINT mtime counter (64 bit) at 0x1400bff8,
 * 24 MHz. On rv32 only the low word of the counter is read; the compare
 * arithmetic is wrap safe because one tick period is far below 2^32 cycles.
 * The compare register is updated high word first, the way the CLINT latches
 * it, so a partially written value can never fire.
 */
#include <rtthread.h>
#include <rthw.h>
#include "riscv-ops.h"

#define CLINT_MTIME_LO  (*(volatile rt_uint32_t *)0x1400BFF8u)
#define CLINT_MTIMECMP_LO (*(volatile rt_uint32_t *)0x14004000u)
#define CLINT_MTIMECMP_HI (*(volatile rt_uint32_t *)0x14004004u)

static unsigned long tick_interval;

void rt_tick_interrupt_clear(void);

void drv_systick_isr(void)
{
    rt_tick_interrupt_clear();
    rt_interrupt_enter();
    rt_tick_increase();
    rt_interrupt_leave();
}

void rt_tick_interrupt_clear(void)
{
    /* high word first: the low write latches the pair */
    CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
    CLINT_MTIMECMP_LO = CLINT_MTIME_LO + tick_interval;
    CLINT_MTIMECMP_HI = 0u;
}

void drv_systick_init(unsigned long interval)
{
    tick_interval = interval;
    rt_tick_interrupt_clear();

    /* MIE in mstatus (mie has timer + external from the startup) */
    unsigned long mstatus = read_csr(mstatus);
    mstatus |= 0x8u;
    write_csr(mstatus, mstatus);
}
