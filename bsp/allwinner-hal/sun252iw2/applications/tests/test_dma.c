/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DMA controller, memory to memory through the device model channel API: a copy
 * of 64 KiB must arrive intact and complete once; the channels of the second
 * interrupt group (8..11) are completed by polling and are checked as well.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "test.h"

#define LEN 65536u

static volatile rt_size_t done_size;
static volatile int done_count;

static void dma_done(struct rt_dma_chan *chan, rt_size_t size)
{
    done_size = size;
    done_count++;
}

static struct rt_dma_chan *open_chan(void)
{
    struct rt_dma_slave_config conf = { .direction = RT_DMA_MEM_TO_MEM };
    struct rt_dma_chan *chan = rt_dma_chan_request(rt_console_get_device(), RT_NULL);

    if (rt_is_err_or_null(chan))
    {
        return RT_NULL;
    }
    if (rt_dma_chan_config(chan, &conf))
    {
        rt_dma_chan_release(chan);
        return RT_NULL;
    }
    chan->callback = dma_done;
    return chan;
}

/* copy with the channel and compare */
static rt_bool_t copy_ok(struct test_ctx *c, struct rt_dma_chan *chan, const char *what)
{
    rt_uint8_t *src = rt_malloc_align(LEN, 64), *dst = rt_malloc_align(LEN, 64);
    struct rt_dma_slave_transfer t = { 0 };
    rt_bool_t ok = RT_FALSE;
    rt_uint32_t i;
    char msg[64];

    if (!src || !dst)
    {
        rt_kprintf("no memory\n");
        goto out;
    }
    for (i = 0; i < LEN; i++)
        src[i] = (rt_uint8_t)(i * 7u + 3u);
    memset(dst, 0, LEN);
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH | RT_HW_CACHE_INVALIDATE, dst, LEN);

    t.src_addr = (rt_ubase_t)src;
    t.dst_addr = (rt_ubase_t)dst;
    t.buffer_len = LEN;
    done_count = 0;
    done_size = 0;
    rt_snprintf(msg, sizeof(msg), "%s: prepare", what);
    TEST_CHECK(c, rt_dma_prep_memcpy(chan, &t) == RT_EOK, msg);
    rt_dma_chan_start(chan);
    for (i = 0; i < 200 && !done_count; i++)
        rt_thread_mdelay(5);
    rt_snprintf(msg, sizeof(msg), "%s: completion callback", what);
    TEST_CHECK(c, done_count == 1 && done_size == LEN, msg);
    rt_snprintf(msg, sizeof(msg), "%s: data equal", what);
    ok = memcmp(src, dst, LEN) == 0;
    TEST_CHECK(c, ok, msg);
out:
    if (src) rt_free_align(src);
    if (dst) rt_free_align(dst);
    return ok;
}

static int test_dma(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    struct rt_dma_chan *chan = open_chan();

    TEST_CHECK(&c, chan != RT_NULL, "request and configure a memory to memory channel");
    if (chan)
    {
        copy_ok(&c, chan, "copy 64 KiB");
        copy_ok(&c, chan, "second copy on the same channel");
        rt_dma_chan_release(chan);
    }
    return test_summary("dma", &c);
}
MSH_CMD_EXPORT(test_dma, DMA memory to memory copy);

/*
 * Take every channel nobody owns: they are handed out in ascending order, so the last four are the
 * channels 8..11 whose interrupt does not fire (completed by polling). The drivers of the board hold
 * their own channels from the device tree, those never show up here.
 */
static int test_dma_hi(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    struct rt_dma_chan *chans[12];
    int i, n = 0;

    for (i = 0; i < 12; i++)
    {
        struct rt_dma_chan *ch = rt_dma_chan_request(rt_console_get_device(), RT_NULL);

        if (rt_is_err_or_null(ch))
            break;
        chans[n++] = ch;
    }
    TEST_CHECK(&c, n >= 4, "at least four free channels (8..11 among them)");
    for (i = n >= 4 ? n - 4 : n; i < n; i++)
    {
        char msg[40];
        struct rt_dma_slave_config conf = { .direction = RT_DMA_MEM_TO_MEM };

        rt_dma_chan_config(chans[i], &conf);
        chans[i]->callback = dma_done;
        rt_snprintf(msg, sizeof(msg), "channel %d", 8 + i - (n - 4));
        copy_ok(&c, chans[i], msg);
    }
    for (i = 0; i < n; i++)
        rt_dma_chan_release(chans[i]);
    return test_summary("dma_hi", &c);
}
MSH_CMD_EXPORT(test_dma_hi, DMA channels 8..11 (completed by polling));
