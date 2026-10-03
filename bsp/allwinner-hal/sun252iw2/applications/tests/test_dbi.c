/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MIPI DBI: the pins stay with the RGB panel, so the transfers run with no
 * panel behind them. What is checked is that the controller clocks the data
 * out and reports completion: a short command (FIFO path), a command with
 * data and a frame sized block (DMA path).
 */
#include <rtthread.h>
#include <mipi_dbi/mipi-dbi-sun252i.h>
#include "test.h"

static int test_dbi(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_uint8_t *buf = rt_malloc_align(32 * 1024, 64);
    rt_uint8_t small[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    rt_uint32_t t0, us;
    rt_uint32_t i;

    TEST_CHECK(&c, sun252i_dbi_ready(), "DBI probed on SPI1");
    for (i = 0; i < 32 * 1024; i++) buf[i] = (rt_uint8_t)i;
    TEST_CHECK(&c, sun252i_dbi_command_write(0x2c, RT_NULL, 0) == 0, "command without data (FIFO path)");
    TEST_CHECK(&c, sun252i_dbi_command_write(0x2a, small, sizeof(small)) == 0, "command with 10 data bytes (FIFO path)");
    t0 = test_mtime();
    TEST_CHECK(&c, sun252i_dbi_write_display(buf, 32 * 1024) == 0, "32 KiB frame data (DMA path)");
    us = (test_mtime() - t0) / 24;
    rt_kprintf("dbi: 32 KiB in %u us = %u KiB/s\n", us, us ? (rt_uint32_t)((rt_uint64_t)32 * 1000000u / us) : 0);
    /* the module clock divides 24 MHz by powers of two: 3 MHz for the 5 MHz request, 32 KiB take about 87 ms */
    TEST_CHECK(&c, us > 70000 && us < 150000, "transfer time matches the 3 MHz bit clock");
    rt_free_align(buf);
    return test_summary("dbi", &c);
}
MSH_CMD_EXPORT(test_dbi, MIPI DBI transfers without a panel);
