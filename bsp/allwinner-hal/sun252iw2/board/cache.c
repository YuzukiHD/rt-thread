/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cache maintenance of the C907 with the T-Head cache instructions (xtheadcmo): by physical
 * address for ranges, whole cache for enable/disable, and the on/off switches in the hardware
 * cache register (MHCR). The DMA drivers clean the data a device reads and invalidate the data
 * it wrote.
 */
#include <rtthread.h>
#include <rthw.h>

#define CACHE_LINE      64u

#define MHCR_IE         (1u << 0)       /* instruction cache enable */
#define MHCR_DE         (1u << 1)       /* data cache enable */

#define MCOR_ICACHE     (1u << 0)
#define MCOR_DCACHE     (1u << 1)
#define MCOR_INV        (1u << 4)       /* invalidate the selected caches */
#define MCOR_BHT_INV    (1u << 16)
#define MCOR_BTB_INV    (1u << 17)

#define CSR_MHCR        "0x7c1"
#define CSR_MCOR        "0x7c2"

static rt_ubase_t mhcr_read(void)
{
    rt_ubase_t v;

    __asm__ volatile ("csrr %0, " CSR_MHCR : "=r"(v));

    return v;
}

static void line_range(void *addr, int size, rt_ubase_t *start, rt_ubase_t *end)
{
    *start = (rt_ubase_t)addr & ~(CACHE_LINE - 1u);
    *end = ((rt_ubase_t)addr + (rt_ubase_t)size + CACHE_LINE - 1u) & ~(CACHE_LINE - 1u);
}

void rt_hw_cpu_dcache_ops(int ops, void *addr, int size)
{
    rt_ubase_t a, end;

    if (size <= 0)
    {
        return;
    }
    line_range(addr, size, &a, &end);

    __asm__ volatile ("fence" ::: "memory");
    for (; a < end; a += CACHE_LINE)
    {
        if ((ops & RT_HW_CACHE_FLUSH) && (ops & RT_HW_CACHE_INVALIDATE))
        {
            __asm__ volatile ("th.dcache.cipa %0" :: "r"(a) : "memory");
        }
        else if (ops & RT_HW_CACHE_FLUSH)
        {
            __asm__ volatile ("th.dcache.cpa %0" :: "r"(a) : "memory");
        }
        else if (ops & RT_HW_CACHE_INVALIDATE)
        {
            __asm__ volatile ("th.dcache.ipa %0" :: "r"(a) : "memory");
        }
    }
    __asm__ volatile ("fence" ::: "memory");
}

void rt_hw_cpu_icache_ops(int ops, void *addr, int size)
{
    rt_ubase_t a, end;

    if (ops & RT_HW_CACHE_INVALIDATE)
    {
        if (size > 0)
        {
            line_range(addr, size, &a, &end);
            __asm__ volatile ("fence" ::: "memory");
            for (; a < end; a += CACHE_LINE)
            {
                __asm__ volatile ("th.icache.ipa %0" :: "r"(a) : "memory");
            }
        }
        else
        {
            __asm__ volatile ("th.icache.iall" ::: "memory");
        }
    }
    __asm__ volatile ("fence.i" ::: "memory");
}

/* the whole data cache: write back, invalidate, or both */
void rt_hw_cpu_dcache_clean_all(void)
{
    __asm__ volatile ("fence" ::: "memory");
    __asm__ volatile ("th.dcache.call" ::: "memory");
    __asm__ volatile ("fence" ::: "memory");
}

void rt_hw_cpu_dcache_invalidate_all(void)
{
    __asm__ volatile ("fence" ::: "memory");
    __asm__ volatile ("th.dcache.iall" ::: "memory");
    __asm__ volatile ("fence" ::: "memory");
}

void rt_hw_cpu_dcache_clean_invalidate_all(void)
{
    __asm__ volatile ("fence" ::: "memory");
    __asm__ volatile ("th.dcache.ciall" ::: "memory");
    __asm__ volatile ("fence" ::: "memory");
}

void rt_hw_cpu_icache_invalidate_all(void)
{
    __asm__ volatile ("th.icache.iall" ::: "memory");
    __asm__ volatile ("fence.i" ::: "memory");
}

/* the caches are invalidated before they are switched on, so no stale line survives a disabled period */
void rt_hw_cpu_icache_enable(void)
{
    rt_ubase_t v = MCOR_INV | MCOR_ICACHE | MCOR_BHT_INV | MCOR_BTB_INV;

    if (mhcr_read() & MHCR_IE)
    {
        return;
    }
    __asm__ volatile ("csrw " CSR_MCOR ", %0" :: "r"(v));
    __asm__ volatile ("fence.i" ::: "memory");
    __asm__ volatile ("csrs " CSR_MHCR ", %0" :: "r"(MHCR_IE));
    __asm__ volatile ("fence" ::: "memory");
}

void rt_hw_cpu_icache_disable(void)
{
    __asm__ volatile ("csrc " CSR_MHCR ", %0" :: "r"(MHCR_IE));
    __asm__ volatile ("fence.i" ::: "memory");
}

rt_base_t rt_hw_cpu_icache_status(void)
{
    return !!(mhcr_read() & MHCR_IE);
}

void rt_hw_cpu_dcache_enable(void)
{
    rt_ubase_t v = MCOR_INV | MCOR_DCACHE;

    if (mhcr_read() & MHCR_DE)
    {
        return;
    }
    __asm__ volatile ("csrw " CSR_MCOR ", %0" :: "r"(v));
    __asm__ volatile ("fence" ::: "memory");
    __asm__ volatile ("csrs " CSR_MHCR ", %0" :: "r"(MHCR_DE));
    __asm__ volatile ("fence" ::: "memory");
}

/* dirty lines go to memory before the cache is switched off */
void rt_hw_cpu_dcache_disable(void)
{
    if (!(mhcr_read() & MHCR_DE))
    {
        return;
    }
    rt_hw_cpu_dcache_clean_invalidate_all();
    __asm__ volatile ("csrc " CSR_MHCR ", %0" :: "r"(MHCR_DE));
    __asm__ volatile ("fence" ::: "memory");
}

rt_base_t rt_hw_cpu_dcache_status(void)
{
    return !!(mhcr_read() & MHCR_DE);
}
