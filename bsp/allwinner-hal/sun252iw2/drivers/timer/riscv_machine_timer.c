/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Kernel tick of the C907: the CLINT mtime counter (64 bit) at 0x1400bff8,
 * 24 MHz. The low word wraps after about 179 s, so the compare value is a
 * full 64 bit sum. The compare register is updated high word first, the way
 * the CLINT latches it, so a partially written value can never fire.
 */
#include <rtthread.h>
#include <rthw.h>
#include <drivers/clock_time.h>
#include "riscv-ops.h"

#define CLINT_MTIME_LO  (*(volatile rt_uint32_t *)0x1400BFF8u)
#define CLINT_MTIME_HI  (*(volatile rt_uint32_t *)0x1400BFFCu)
#define CLINT_MTIMECMP_LO (*(volatile rt_uint32_t *)0x14004000u)
#define CLINT_MTIMECMP_HI (*(volatile rt_uint32_t *)0x14004004u)

#define MTIME_FREQ      24000000u

static unsigned long tick_interval;

/* the free running counter, for the clock time interface (rt_clock_time_get_counter) */
static rt_uint64_t mtime_get_freq(struct rt_clock_time_device *dev)
{
    return MTIME_FREQ;
}

static rt_uint64_t mtime_get_counter(struct rt_clock_time_device *dev)
{
    rt_uint32_t hi, lo;

    do
    {
        hi = CLINT_MTIME_HI;
        lo = CLINT_MTIME_LO;
    } while (hi != CLINT_MTIME_HI);

    return ((rt_uint64_t)hi << 32) | lo;
}

static const struct rt_clock_time_ops mtime_ops =
{
    .get_freq = mtime_get_freq,
    .get_counter = mtime_get_counter,
};

static struct rt_clock_time_device mtime_dev = { .ops = &mtime_ops };

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
    rt_uint32_t hi, lo, next_hi, next_lo;

    /* a consistent 64 bit read: the high word must not change around the low read */
    do
    {
        hi = CLINT_MTIME_HI;
        lo = CLINT_MTIME_LO;
    } while (hi != CLINT_MTIME_HI);

    next_lo = lo + (rt_uint32_t)tick_interval;
    next_hi = hi + (next_lo < lo ? 1u : 0u);

    /* high word first: the low write latches the pair */
    CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
    CLINT_MTIMECMP_LO = next_lo;
    CLINT_MTIMECMP_HI = next_hi;
}

void drv_systick_init(unsigned long interval)
{
    tick_interval = interval;
    rt_clock_time_device_register(&mtime_dev, RT_NULL, RT_CLOCK_TIME_CAP_SOURCE);
    rt_tick_interrupt_clear();

    /* MIE in mstatus (mie has timer + external from the startup) */
    unsigned long mstatus = read_csr(mstatus);
    mstatus |= 0x8u;
    write_csr(mstatus, mstatus);
}
