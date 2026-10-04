/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_all: the tests that need nothing wired and cannot stop the board, one
 * after the other, with one line of result each. Not included: test_trap and
 * test_wdt_reset (they stop or reset the chip), test_spi write (writes the
 * flash), test_sd (needs a card), test_spif (needs SUN252I_BOARD=evb-spif), usb_* (need the host side).
 */
#include <rtthread.h>
#include <finsh.h>
#include <string.h>

static int test_all(int argc, char **argv)
{
    static const char *const names[] = {
        "test_kernel", "test_fpu", "test_cache", "test_gpio", "test_dma", "test_dma_hi", "test_wdt",
        "test_i2c", "test_spi", "test_pwm", "test_backlight", "test_adc", "test_audio", "test_mic", "test_i2s",
        "test_owa", "test_dbi", "test_display", "test_g2d", "test_vdec", "test_mbus", "test_sid", "test_cpu", "test_sd",
    };
    unsigned i;
    int failed = 0;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        extern int msh_exec(char *cmd, rt_size_t length);
        char cmd[24];
        int r;

        strcpy(cmd, names[i]);
        r = msh_exec(cmd, strlen(cmd));
        rt_kprintf("ALL %-14s %s\n", names[i], r == 0 ? "ok" : (r == -1 ? "FAIL or missing" : "FAIL"));
        if (r != 0) failed++;
    }
    rt_kprintf("TEST all: %s (%d failed)\n", failed ? "FAIL" : "PASS", failed);
    return failed ? -1 : 0;
}
MSH_CMD_EXPORT(test_all, run every self contained test in turn);
