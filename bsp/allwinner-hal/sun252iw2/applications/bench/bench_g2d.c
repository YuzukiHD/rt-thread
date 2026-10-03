/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * G2D speed: 720p fill, copy, format conversion, scale, rotate and blend, hardware time only; and the same
 * table at several module clocks. Needs BSP_USING_BENCH.
 */
#include <rtthread.h>
#include <string.h>
#include <g2d/g2d.h>

/*
 * g2d_bench: 720p operations, hardware time only (from the start of the command list to the end
 * interrupt; no cache maintenance, no queueing, no thread switch). The table runs three times:
 * as the system is set up, with the limit inside the G2D off, and with the G2D also at the highest
 * priority of the memory bus without a bandwidth limit.
 */
#include "mbus-sun252i.h"

#define BENCH_LOOPS 20
#define MBUS_PMU_G2D 9

static rt_uint32_t bench_run(const struct g2d_op *op, int loops, rt_uint32_t *min, rt_uint32_t *max, rt_uint32_t *traffic)
{
    rt_uint32_t i, t, sum = 0, tr0 = mbus_traffic(MBUS_PMU_G2D);

    *min = ~0u;
    *max = 0;
    for (i = 0; i < loops; i++)
    {
        if (g2d_run(op, rt_tick_from_millisecond(2000)) != 0)
            return 0;
        t = g2d_last_hw_time_x100us();
        sum += t;
        *min = t < *min ? t : *min;
        *max = t > *max ? t : *max;
    }
    if (traffic)
        *traffic = (mbus_traffic(MBUS_PMU_G2D) - tr0) / loops;

    return sum / loops;
}

static void bench_report(const char *name, const struct g2d_op *op, rt_uint32_t pix, rt_uint32_t bytes)
{
    rt_uint32_t t, min, max, traffic;

    t = bench_run(op, BENCH_LOOPS, &min, &max, &traffic);
    if (!t)
    {
        rt_kprintf("bench %-28s failed\n", name);
        return;
    }
    /* t is in 1/100 us: Mpix/s = pix * 100 / t, bytes * 100 / t is MB/s (10^6 bytes) */
    rt_kprintf("bench %-26s hw %3u.%02u ms (min %u.%02u max %u.%02u) %4u Mpix/s, %4u MB/s (bus counter %u KB per run)\n", name,
               t / 100000, (t / 1000) % 100, min / 100000, (min / 1000) % 100, max / 100000, (max / 1000) % 100,
               (rt_uint32_t)((rt_uint64_t)pix * 100u / t), (rt_uint32_t)((rt_uint64_t)bytes * 100u / t), traffic / 1024u);
}

static void bench_table(void *a, void *b)
{
    const rt_uint32_t BW = 1280, BH = 720, px = BW * BH;
    struct g2d_rect full = {0, 0, BW, BH};
    struct g2d_op op;

    rt_memset(&op, 0, sizeof(op));
    op.flags = G2D_FLAG_NO_CACHE_OPS;
    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.dst_rect = full;

    op.type = G2D_OP_FILL;
    op.color = 0xff336699;
    bench_report("fill ARGB8888", &op, px, px * 4);

    op.type = G2D_OP_BLIT;
    op.src = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {a}, {BW * 4}};
    op.src_rect = full;
    bench_report("copy ARGB8888", &op, px, px * 8);

    op.src = (struct g2d_surface){G2D_PIXFMT_RGB565, BW, BH, {a}, {BW * 2}};
    bench_report("RGB565 -> ARGB8888", &op, px, px * 6);

    op.src = (struct g2d_surface){G2D_PIXFMT_NV12, BW, BH, {a, (rt_uint8_t *)a + BW * BH}, {BW, BW}};
    bench_report("NV12 -> ARGB8888", &op, px, px * 4 + px * 3 / 2);

    op.src = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {a}, {BW * 4}};
    op.src_rect = (struct g2d_rect){0, 0, BW / 2, BH / 2};
    bench_report("scale up 640x360 -> 720p", &op, px, px * 4 + px);

    op.src_rect = full;
    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BH, BW, {b}, {BH * 4}};
    op.dst_rect = (struct g2d_rect){0, 0, BH, BW};
    op.rotation = G2D_ROTATE_90;
    bench_report("rotate 90", &op, px, px * 8);
    op.rotation = G2D_ROTATE_0;

    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.dst_rect = full;
    op.flags |= G2D_FLIP_H;
    bench_report("flip horizontal", &op, px, px * 8);
    op.flags &= ~G2D_FLIP_H;

    op.type = G2D_OP_BLEND;
    op.bg = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.bg_rect = full;
    op.blend.mode = G2D_BLEND_SRC_OVER;
    op.blend.fg_alpha_mode = G2D_ALPHA_PIXEL;
    op.blend.bg_alpha_mode = G2D_ALPHA_PIXEL;
    bench_report("blend src-over", &op, px, px * 12);
}

/* find the bus master of the G2D: the one whose bandwidth limit slows a copy down */
static int bench_find_master(void *a, void *b)
{
    const rt_uint32_t BW = 1280, BH = 720;
    struct g2d_op op;
    rt_uint32_t base, t, min, max, m, old;

    rt_memset(&op, 0, sizeof(op));
    op.flags = G2D_FLAG_NO_CACHE_OPS;
    op.type = G2D_OP_BLIT;
    op.src = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {a}, {BW * 4}};
    op.src_rect = (struct g2d_rect){0, 0, BW, BH};
    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.dst_rect = op.src_rect;
    base = bench_run(&op, 3, &min, &max, RT_NULL);
    for (m = 0; m < 40; m++)    /* the MBUS has 40 masters */
    {
        if (m == 39)    /* the CPU */
            continue;
        mbus_get_limit(m, &old);
        mbus_set_limit(m, 50);
        t = bench_run(&op, 2, &min, &max, RT_NULL);
        mbus_set_limit(m, old);
        if (t == 0 || t > base * 3u / 2u)    /* 0: the job ran into its timeout */
            return (int)m;
    }

    return -1;
}

static int g2d_bench(int argc, char **argv)
{
    const rt_uint32_t BW = 1280, BH = 720;
    void *a = rt_malloc_align(BW * BH * 4, 64), *b = rt_malloc_align(BW * BH * 4, 64);
    rt_uint32_t old_limit = g2d_get_ddr_limit(), old_prio = 0, old_mbps = 0;
    int master;

    if (!a || !b)
    {
        rt_kprintf("g2d_bench: no memory for two 720p ARGB8888 buffers\n");
        goto out;
    }
    /* the sources are only read and the tests run one after the other: every format reuses the memory of a */
    memset(a, 0x80, BW * BH * 4);
    memset(b, 0x40, BW * BH * 4);

    master = bench_find_master(a, b);
    rt_kprintf("g2d_bench: 720p, G2D limit level %u, bus master of the G2D: %d\n", old_limit, master);

    rt_kprintf("--- as set up\n");
    bench_table(a, b);

    rt_kprintf("--- limit inside the G2D off\n");
    g2d_set_ddr_limit(0);
    bench_table(a, b);

    if (master >= 0)
    {
        mbus_get_priority(master, &old_prio);
        mbus_get_limit(master, &old_mbps);
        mbus_set_priority(master, 3);
        mbus_set_limit(master, 0);
        rt_kprintf("--- and the memory bus: G2D at priority 3, no bandwidth limit\n");
        bench_table(a, b);
        mbus_set_priority(master, old_prio);
        mbus_set_limit(master, old_mbps);
    }
    g2d_set_ddr_limit(old_limit);
out:
    rt_free_align(a);
    rt_free_align(b);

    return 0;
}
MSH_CMD_EXPORT(g2d_bench, 720p G2D operations: hardware time per operation);

/* a fill must reach the memory: the first and last word of the buffer carry the colour */
static rt_bool_t bench_fill_ok(void *b)
{
    const rt_uint32_t BW = 1280, BH = 720;
    struct g2d_op op;
    rt_uint32_t *w = b;

    rt_memset(&op, 0, sizeof(op));
    op.type = G2D_OP_FILL;
    op.color = 0xff12ab34;
    op.dst = (struct g2d_surface){G2D_PIXFMT_ARGB8888, BW, BH, {b}, {BW * 4}};
    op.dst_rect = (struct g2d_rect){0, 0, BW, BH};
    w[0] = w[BW * BH - 1] = 0;
    sunxi_dcache_flush(b, BW * BH * 4);
    if (g2d_run(&op, rt_tick_from_millisecond(2000)) != 0)
        return RT_FALSE;

    return w[0] == 0xff12ab34u && w[BW * BH - 1] == 0xff12ab34u;
}

/* g2d_bench_clk: the 720p table at each module clock, up to the highest the clock source gives */
static int g2d_bench_clk(int argc, char **argv)
{
    static const rt_uint32_t rates[] = {150000000u, 200000000u, 300000000u, 400000000u, 600000000u, 1200000000u};
    const rt_uint32_t BW = 1280, BH = 720;
    void *a = rt_malloc_align(BW * BH * 4, 64), *b = rt_malloc_align(BW * BH * 4, 64);
    rt_uint32_t old = g2d_get_clock_rate(), i, got;

    if (!a || !b)
    {
        rt_kprintf("g2d_bench_clk: no memory\n");
        goto out;
    }
    memset(a, 0x80, BW * BH * 4);
    memset(b, 0x40, BW * BH * 4);
    g2d_set_ddr_limit(0);
    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    {
        got = g2d_set_clock_rate(rates[i]);
        rt_kprintf("--- module clock %u MHz requested, %u MHz set, output %s\n", rates[i] / 1000000u, got / 1000000u,
                   bench_fill_ok(b) ? "correct" : "WRONG");
        bench_table(a, b);
    }
out:
    g2d_set_clock_rate(old ? old : 300000000u);
    g2d_set_ddr_limit(0x90);
    rt_free_align(a);
    rt_free_align(b);

    return 0;
}
MSH_CMD_EXPORT(g2d_bench_clk, 720p G2D table at module clocks from 150 MHz to the highest);
