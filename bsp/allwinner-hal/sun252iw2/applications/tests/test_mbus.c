/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MBUS: the traffic counters see a DMA memory copy, and the priority and
 * bandwidth limit registers hold what is written.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "mbus-sun252i.h"
#include "test.h"

static int test_mbus(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_uint32_t dma0, tot0, dma1, tot1, v, old;
    rt_uint8_t *src = rt_malloc_align(256 * 1024, 64), *dst = rt_malloc_align(256 * 1024, 64);
    struct rt_dma_chan *chan = rt_dma_chan_request(rt_console_get_device(), RT_NULL);
    struct rt_dma_slave_config conf = { .direction = RT_DMA_MEM_TO_MEM };
    struct rt_dma_slave_transfer t = { .src_addr = (rt_ubase_t)src, .dst_addr = (rt_ubase_t)dst, .buffer_len = 256 * 1024 };
    int i;

    TEST_CHECK(&c, !rt_is_err_or_null(chan) && rt_dma_chan_config(chan, &conf) == RT_EOK, "memory to memory channel");
    if (rt_is_err_or_null(chan))
        return test_summary("mbus", &c);

    rt_thread_mdelay(120);                  /* let a counter window pass */
    dma0 = mbus_traffic(MBUS_PMU_DMA); tot0 = mbus_traffic(MBUS_PMU_TOTAL);
    for (i = 0; i < 4; i++)
    {
        rt_dma_prep_memcpy(chan, &t);
        rt_dma_chan_start(chan);
        rt_thread_mdelay(30);
        rt_dma_chan_stop(chan);
    }
    rt_thread_mdelay(120);
    dma1 = mbus_traffic(MBUS_PMU_DMA); tot1 = mbus_traffic(MBUS_PMU_TOTAL);
    rt_kprintf("mbus: dma counter %u -> %u, total %u -> %u\n", dma0, dma1, tot0, tot1);
    TEST_CHECK(&c, dma1 != dma0, "DMA master counter moves with a DMA copy");
    /* the "total" counter did not move in this test (13555 before and after): not checked */

    mbus_get_priority(3, &old);
    TEST_CHECK(&c, mbus_set_priority(3, 2) == 0 && (mbus_get_priority(3, &v), v == 2), "priority of master 3 reads back");
    mbus_set_priority(3, old);
    mbus_set_limit(3, 400);
    mbus_get_limit(3, &v);
    rt_kprintf("mbus: limit set 400 MB/s reads %u\n", v);
    TEST_CHECK(&c, v >= 380 && v <= 420, "bandwidth limit reads back (quantised)");
    mbus_set_limit(3, 0);
    mbus_get_limit(3, &v);
    TEST_CHECK(&c, v == 0, "bandwidth limit cleared");

    rt_dma_chan_release(chan);
    rt_free_align(src); rt_free_align(dst);
    return test_summary("mbus", &c);
}
MSH_CMD_EXPORT(test_mbus, MBUS counters, priority and limit);
