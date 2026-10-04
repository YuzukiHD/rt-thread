/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPIF on the EVB flash (board evb-spif): identification, sample point tuning and its
 * storage, read speed, erase/program/verify, and the XIP window (reads, flash writes while it
 * is mapped). Needs the flash controller device tree (SUN252I_BOARD=evb-spif) and is not part
 * of test_all. The areas used are blank space of the board flash.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include "spif-sun252i.h"
#include "sunxi-util.h"
#include "test.h"

#define SCRATCH_OFF 0x840000u
#define XIP_OFF     0x880000u
#define CHUNK       (64u * 1024u)

static rt_uint8_t buf_a[CHUNK] __attribute__((aligned(64)));
static rt_uint8_t buf_b[CHUNK] __attribute__((aligned(64)));

static void prbs(rt_uint8_t *p, rt_size_t n, rt_uint32_t seed)
{
    rt_uint32_t x = seed ? seed : 1u;
    rt_size_t i;

    for (i = 0; i < n; i++)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = x >> 11;
    }
}

static rt_bool_t is_blank(const rt_uint8_t *p, rt_size_t n)
{
    rt_size_t i;

    for (i = 0; i < n; i++)
    {
        if (p[i] != 0xff)
        {
            return RT_FALSE;
        }
    }

    return RT_TRUE;
}

static int test_spif(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    struct sun252i_spif_info info;
    struct sun252i_spif_tune_result tr;
    rt_uint8_t mode, delay;
    rt_tick_t t0, ms;
    rt_uint32_t i;
    int ret;

    if (sun252i_spif_get_info(&info) != RT_EOK)
    {
        rt_kprintf("spif: not probed (build the board evb-spif)\n");
        TEST_CHECK(&c, 0, "flash controller probed");
        return test_summary("spif", &c);
    }
    rt_kprintf("spif: controller %08x, flash %02x%02x%02x %u KiB, %u Hz, quad %d dtr %d, sample %s mode %u delay %u\n",
               info.version, info.jedec_id[0], info.jedec_id[1], info.jedec_id[2], info.size / 1024u,
               info.frequency, info.quad, info.dtr, info.sample_tuned ? "tuned" : "none", info.sample_mode,
               info.sample_delay);
    TEST_CHECK(&c, info.size >= 1024u * 1024u, "flash size from the JEDEC ID");
    TEST_CHECK(&c, info.quad, "quad enabled");

    ret = sun252i_spif_tune(&tr);
    rt_kprintf("spif: tune %d, %u Hz, mode %u delay %u window %u+%u\n", ret, tr.frequency, tr.mode, tr.delay,
               tr.window_start, tr.window_len);
    TEST_CHECK(&c, ret == 0, "tuning finds a window");
    TEST_CHECK(&c, tr.window_len >= 8, "window of at least 8 steps");

    /* the stored sample point comes back after another one was applied */
    sun252i_spif_get_info(&info);
    mode = info.sample_mode;
    delay = info.sample_delay;
    TEST_CHECK(&c, sun252i_spif_params_save() == RT_EOK, "sample point saved");
    sun252i_spif_set_sample((mode + 1) % 3, (delay + 7) % 64);
    TEST_CHECK(&c, sun252i_spif_params_load() == RT_EOK, "sample point loaded");
    sun252i_spif_get_info(&info);
    TEST_CHECK(&c, info.sample_mode == mode && info.sample_delay == delay, "loaded point is the saved one");

    /* reads are repeatable, aligned and unaligned destinations agree */
    TEST_CHECK(&c, sun252i_spif_read(0, buf_a, CHUNK) == (rt_ssize_t)CHUNK, "read 64 KiB");
    TEST_CHECK(&c, sun252i_spif_read(0, buf_b, CHUNK) == (rt_ssize_t)CHUNK && memcmp(buf_a, buf_b, CHUNK) == 0,
               "second read identical");
    TEST_CHECK(&c, sun252i_spif_read(0, buf_b + 1, CHUNK - 64) == (rt_ssize_t)(CHUNK - 64) &&
               memcmp(buf_a, buf_b + 1, CHUNK - 64) == 0, "unaligned destination identical");

    t0 = rt_tick_get_millisecond();
    for (i = 0; i < 32; i++)
    {
        sun252i_spif_read(i * CHUNK, buf_a, CHUNK);
    }
    ms = rt_tick_get_millisecond() - t0;
    rt_kprintf("spif: read 2 MiB in %u ms (%u KB/s)\n", (rt_uint32_t)ms, ms ? (rt_uint32_t)(2048u * 1000u / ms) : 0u);
    TEST_CHECK(&c, ms != 0 && 2048u * 1000u / ms > 10000u, "read faster than 10 MB/s");

    /* erase, program, verify */
    TEST_CHECK(&c, sun252i_spif_erase(SCRATCH_OFF, 4 * CHUNK) == RT_EOK, "erase 256 KiB");
    for (i = 0; i < 4; i++)
    {
        sun252i_spif_read(SCRATCH_OFF + i * CHUNK, buf_a, CHUNK);
        TEST_CHECK(&c, is_blank(buf_a, CHUNK), "erased block reads 0xff");
    }
    prbs(buf_a, CHUNK, 0x1234u);
    TEST_CHECK(&c, sun252i_spif_write(SCRATCH_OFF + 100, buf_a, CHUNK - 100) == (rt_ssize_t)(CHUNK - 100),
               "program across pages from an odd offset");
    memset(buf_b, 0, sizeof(buf_b));
    sun252i_spif_read(SCRATCH_OFF + 100, buf_b, CHUNK - 100);
    TEST_CHECK(&c, memcmp(buf_a, buf_b, CHUNK - 100) == 0, "programmed data reads back");
    TEST_CHECK(&c, sun252i_spif_erase(SCRATCH_OFF + 1, 4096) == -RT_EINVAL, "unaligned erase refused");
    TEST_CHECK(&c, sun252i_spif_read(info.size - 4, buf_b, 8) < 0, "read past the end refused");

    /* the XIP window shows the flash, also while commands change it */
    TEST_CHECK(&c, sun252i_spif_erase(XIP_OFF, CHUNK) == RT_EOK, "erase the XIP test area");
    prbs(buf_a, CHUNK, 0x9876u);
    sun252i_spif_write(XIP_OFF, buf_a, CHUNK);
    TEST_CHECK(&c, sun252i_spif_xip_enable(XIP_OFF, CHUNK) == RT_EOK, "XIP window mapped");
    {
        const rt_uint8_t *win = sun252i_spif_xip_window();

        t0 = rt_tick_get_millisecond();
        TEST_CHECK(&c, memcmp(win, buf_a, CHUNK) == 0, "window reads the flash");
        ms = rt_tick_get_millisecond() - t0;
        rt_kprintf("spif: window read of 64 KiB took %u ms\n", (rt_uint32_t)ms);
        TEST_CHECK(&c, sun252i_spif_erase(XIP_OFF, 4096) == RT_EOK, "erase while mapped");
        TEST_CHECK(&c, is_blank(win, 4096), "window shows the erased sector");
        prbs(buf_b, 4096, 0x5555u);
        sun252i_spif_write(XIP_OFF, buf_b, 4096);
        TEST_CHECK(&c, memcmp(win, buf_b, 4096) == 0, "window shows the programmed sector");
        TEST_CHECK(&c, memcmp(win + 4096, buf_a + 4096, CHUNK - 4096) == 0, "rest of the window unchanged");
        TEST_CHECK(&c, sun252i_spif_tune(RT_NULL) == -RT_EBUSY, "tuning refused while mapped");
    }
    sun252i_spif_xip_disable();
    sun252i_spif_read(XIP_OFF, buf_a, 4096);
    TEST_CHECK(&c, memcmp(buf_a, buf_b, 4096) == 0, "flash holds the data written while mapped");

    /* leave the test areas blank */
    sun252i_spif_erase(SCRATCH_OFF, 4 * CHUNK);
    sun252i_spif_erase(XIP_OFF, CHUNK);

    return test_summary("spif", &c);
}
MSH_CMD_EXPORT(test_spif, SPIF flash controller tests);
