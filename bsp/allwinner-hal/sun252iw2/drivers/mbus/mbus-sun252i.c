/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Memory bus (MBUS): the traffic counters of the bus masters, the per master
 * priority and bandwidth limit. The counters count bytes inside a window;
 * msh command "mbus_stats [ms]" prints the bandwidth of every master.
 */
#include <rtthread.h>
#include <stdlib.h>
#include <rtdevice.h>
#include <stdlib.h>
#include "mbus-sun252i.h"

#define MBUS_PMU       0x009cu
#define MBUS_MSC       0x0210u
#define MBUS_BWLR      0x0218u
#define MBUS_STRIDE    0x10u
#define MBUS_CLOCK_MHZ 252u
#define MBUS_WINDOW_US 50000u
#define MCGCR_ENABLE   1u

static const char * const pmu_names[MBUS_PMU_COUNT] = {
    "cpu",
    "rv_sys",
    "mahb",
    "dma",
    "ve",
    "ce",
    "tvd",
    "csi",
    "dsp_sys",
    "g2d",
    "di",
    "de",
    "iommu",
    "reserved",
    "other",
    "total",
};

static rt_ubase_t mbus_base;

static rt_uint32_t mrd(rt_uint32_t off)
{
    return HWREG32(mbus_base + off);
}
static void mwr(rt_uint32_t off, rt_uint32_t v)
{
    HWREG32(mbus_base + off) = v;
}

const char *mbus_pmu_name(int counter)
{
    return (counter >= 0 && counter < MBUS_PMU_COUNT) ? pmu_names[counter] : "?";
}

rt_uint32_t mbus_traffic(int counter)
{
    return mrd(MBUS_PMU + 4u + (rt_uint32_t)counter * 4u);
}

int mbus_set_priority(rt_uint32_t master, rt_uint32_t prio)
{
    rt_uint32_t reg = MBUS_MSC + master * MBUS_STRIDE;

    if (prio > 3u)
    {
        return -1;
    }
    mwr(reg, (mrd(reg) & ~(3u << 2)) | (prio << 2));
    return 0;
}

int mbus_get_priority(rt_uint32_t master, rt_uint32_t *prio)
{
    *prio = (mrd(MBUS_MSC + master * MBUS_STRIDE) >> 2) & 3u;
    return 0;
}

int mbus_set_limit(rt_uint32_t master, rt_uint32_t mbps)
{
    rt_uint32_t reg = MBUS_BWLR + master * MBUS_STRIDE;
    rt_uint32_t v = mrd(reg) & ~(0xfffu << 16 | 1u << 31);

    if (mbps)
    {
        v |= ((256u * mbps / MBUS_CLOCK_MHZ) & 0xfffu) << 16 | 1u << 31;
    }
    mwr(reg, v);
    return 0;
}

int mbus_get_limit(rt_uint32_t master, rt_uint32_t *mbps)
{
    rt_uint32_t v = mrd(MBUS_BWLR + master * MBUS_STRIDE);

    *mbps = (v & (1u << 31)) ? ((v >> 16) & 0xfffu) * MBUS_CLOCK_MHZ / 256u : 0u;
    return 0;
}

static rt_err_t mbus_probe(struct rt_platform_device *pdev)
{
    mbus_base = (rt_ubase_t)rt_dm_dev_iomap(&pdev->parent, 0);
    if (!mbus_base)
    {
        return -RT_EIO;
    }
    mwr(MBUS_PMU, mrd(MBUS_PMU) & ~MCGCR_ENABLE);
    mwr(MBUS_PMU, (MBUS_WINDOW_US << 16) | MCGCR_ENABLE);

    return RT_EOK;
}

static int mbus_stats(int argc, char **argv)
{
    rt_uint32_t before[MBUS_PMU_COUNT], after[MBUS_PMU_COUNT], ms = argc > 1 ? atoi(argv[1]) : 1000;
    rt_tick_t t0, t1;
    int i;

    if (ms == 0u)
    {
        ms = 1000u;
    }
    t0 = rt_tick_get();
    for (i = 0; i < MBUS_PMU_COUNT; i++)
    {
        before[i] = mbus_traffic(i);
    }
    rt_thread_mdelay(ms);
    t1 = rt_tick_get();
    for (i = 0; i < MBUS_PMU_COUNT; i++)
    {
        after[i] = mbus_traffic(i);
    }
    rt_kprintf("over %u ms:\n", (rt_uint32_t)((t1 - t0) * 1000u / RT_TICK_PER_SECOND));
    for (i = 0; i < MBUS_PMU_COUNT; i++)
    {
        rt_uint32_t d = after[i] - before[i];
        rt_uint32_t kbps = (rt_uint32_t)((rt_uint64_t)d * RT_TICK_PER_SECOND / (t1 - t0) / 1000u);

        if (d)
        {
            rt_kprintf("%-8s %4u.%02u MB/s\n", mbus_pmu_name(i), kbps / 1000u, (kbps % 1000u) / 10u);
        }
    }
    return 0;
}
MSH_CMD_EXPORT(mbus_stats, bandwidth of every bus master(mbus_stats[ms]));

static const struct rt_ofw_node_id mbus_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-mbus" },
    { /* sentinel */ }
};

static struct rt_platform_driver mbus_driver =
{
    .name = "mbus-sun252i",
    .ids = mbus_ofw_ids,
    .probe = mbus_probe,
};
RT_PLATFORM_DRIVER_EXPORT(mbus_driver);

/* the priority and bandwidth limit registers of every master */
static int mbus_masters(int argc, char **argv)
{
    rt_uint32_t m, prio, mbps;

    for (m = 0; m < 40u; m++)
    {
        mbus_get_priority(m, &prio);
        mbus_get_limit(m, &mbps);
        rt_kprintf("master %2u: MSC %08x BWLR %08x (priority %u, limit %u MB/s)\n", m,
                   mrd(MBUS_MSC + m * MBUS_STRIDE), mrd(MBUS_BWLR + m * MBUS_STRIDE), prio, mbps);
    }

    return 0;
}
MSH_CMD_EXPORT(mbus_masters, priority and limit registers of the 40 bus masters);
