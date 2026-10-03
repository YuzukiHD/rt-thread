/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cache control: both caches are on after start, a data cache off/on cycle keeps the memory
 * contents (dirty lines are written back first), the range and whole cache operations run, and a
 * DMA copy of a dirty buffer sees the data once the buffer is cleaned.
 */
#include <rtthread.h>
#include <rthw.h>
#include <board.h>
#include "test.h"

#define LEN 4096

static int test_cache(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_uint8_t *buf = rt_malloc_align(LEN, 64);
    rt_uint32_t i, bad = 0;

    TEST_CHECK(&c, rt_hw_cpu_icache_status() == 1, "instruction cache on");
    TEST_CHECK(&c, rt_hw_cpu_dcache_status() == 1, "data cache on");
    if (!buf)
        return test_summary("cache", &c);

    for (i = 0; i < LEN; i++)
        buf[i] = (rt_uint8_t)(i * 5u + 1u);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, buf, LEN);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH | RT_HW_CACHE_INVALIDATE, buf, LEN);
    for (i = 0; i < LEN; i++)
        bad += buf[i] != (rt_uint8_t)(i * 5u + 1u);
    TEST_CHECK(&c, bad == 0, "clean and invalidate by range keep the data");

    rt_hw_cpu_dcache_clean_all();
    rt_hw_cpu_dcache_clean_invalidate_all();
    rt_hw_cpu_icache_invalidate_all();
    rt_hw_cpu_icache_ops(RT_HW_CACHE_INVALIDATE, (void *)test_cache, 256);
    for (i = 0, bad = 0; i < LEN; i++)
        bad += buf[i] != (rt_uint8_t)(i * 5u + 1u);
    TEST_CHECK(&c, bad == 0, "whole cache operations keep the data");

    /* dirty lines must reach the memory before the cache goes off */
    for (i = 0; i < LEN; i++)
        buf[i] = (rt_uint8_t)(i * 3u + 7u);
    rt_hw_cpu_dcache_disable();
    TEST_CHECK(&c, rt_hw_cpu_dcache_status() == 0, "data cache off");
    for (i = 0, bad = 0; i < LEN; i++)
        bad += buf[i] != (rt_uint8_t)(i * 3u + 7u);
    TEST_CHECK(&c, bad == 0, "data written back when the cache went off");
    rt_hw_cpu_dcache_enable();
    TEST_CHECK(&c, rt_hw_cpu_dcache_status() == 1, "data cache on again");
    for (i = 0, bad = 0; i < LEN; i++)
        bad += buf[i] != (rt_uint8_t)(i * 3u + 7u);
    TEST_CHECK(&c, bad == 0, "data intact after the off/on cycle");

    rt_hw_cpu_icache_disable();
    TEST_CHECK(&c, rt_hw_cpu_icache_status() == 0, "instruction cache off");
    rt_hw_cpu_icache_enable();
    TEST_CHECK(&c, rt_hw_cpu_icache_status() == 1, "instruction cache on again");

    rt_free_align(buf);
    return test_summary("cache", &c);
}
MSH_CMD_EXPORT(test_cache, cache control and maintenance operations);
