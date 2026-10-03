/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S0 internal loopback: a counting pattern goes round the transmit ring,
 * the receiver ring must hold the same frames (a rotation of the ring, no gap
 * or repeat), and the frames moved in one second must match the rate.
 */
#include <rtthread.h>
#include <string.h>
#include "i2s-sun252i.h"
#include "test.h"

#define RATE    48000u
#define FRAMES  480u
#define PERIODS 8u

static int test_i2s(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_uint32_t total = FRAMES * PERIODS, n, off, t0, t1, tp, rp, bad = 0;
    rt_int16_t *tx, *rx;
    int r;

    r = sun252i_i2s_open(RATE, FRAMES, PERIODS, RT_TRUE);
    TEST_CHECK(&c, r == 0, "open 48 kHz with the internal loopback");
    if (r) return test_summary("i2s", &c);
    tx = (rt_int16_t *)sun252i_i2s_tx_buffer();
    rx = (rt_int16_t *)sun252i_i2s_rx_buffer();
    for (n = 0; n < total; n++)
    {
        tx[2 * n] = (rt_int16_t)(n + 1);
        tx[2 * n + 1] = (rt_int16_t)(-(rt_int32_t)(n + 1));
    }
    TEST_CHECK(&c, sun252i_i2s_start() == 0, "start rings");
    rt_thread_mdelay(200);
    t0 = test_mtime();
    tp = sun252i_i2s_tx_periods();
    rp = sun252i_i2s_rx_periods();
    rt_thread_mdelay(1000);
    t1 = test_mtime();
    tp = sun252i_i2s_tx_periods() - tp;
    rp = sun252i_i2s_rx_periods() - rp;
    sun252i_i2s_stop();
    {
        rt_uint32_t us = (t1 - t0) / 24;
        rt_uint32_t hz = (rt_uint32_t)((rt_uint64_t)tp * FRAMES * 1000000u / us);
        rt_kprintf("i2s: tx %u periods, rx %u periods in %u us = %u frames/s\n", tp, rp, us, hz);
        TEST_CHECK(&c, hz > RATE * 99 / 100 && hz < RATE * 101 / 100, "frame rate within 1 percent of 48 kHz");
        TEST_CHECK(&c, tp >= rp ? tp - rp <= 1 : rp - tp <= 1, "receiver and transmitter move the same number of periods");
    }
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, rx, total * 4);
    /* find the transmit frame 1 in the receive ring */
    for (off = 0; off < total; off++)
        if (rx[2 * off] == 1 && rx[2 * off + 1] == -1)
            break;
    TEST_CHECK(&c, off < total, "first transmit frame found in the receive ring");
    if (off < total)
    {
        for (n = 0; n < total; n++)
        {
            rt_uint32_t k = (off + n) % total;
            if (rx[2 * k] != tx[2 * n] || rx[2 * k + 1] != tx[2 * n + 1])
                bad++;
        }
        rt_kprintf("i2s: ring rotated by %u frames, %u of %u frames differ\n", off, bad, total);
        TEST_CHECK(&c, bad == 0, "receive ring equals the transmit ring (loopback data intact)");
    }
    sun252i_i2s_close();
    return test_summary("i2s", &c);
}
MSH_CMD_EXPORT(test_i2s, I2S0 internal loopback);
