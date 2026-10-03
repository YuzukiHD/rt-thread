/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watchdog: fed for several periods it must not reset (test_wdt); left
 * alone it must reset the chip (test_wdt_reset: the boot banner reappears).
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "test.h"

static int test_wdt(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_device_t dev = rt_device_find("wdt0");
    rt_uint32_t to = 2, got = 0;
    int i;

    TEST_CHECK(&c, dev != RT_NULL, "wdt0 registered");
    if (!dev) return test_summary("wdt", &c);
    TEST_CHECK(&c, rt_device_open(dev, RT_DEVICE_OFLAG_RDWR) == RT_EOK, "open");
    TEST_CHECK(&c, rt_device_control(dev, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &to) == RT_EOK, "set timeout 2 s");
    TEST_CHECK(&c, rt_device_control(dev, RT_DEVICE_CTRL_WDT_GET_TIMEOUT, &got) == RT_EOK && got == 2, "get timeout");
    TEST_CHECK(&c, rt_device_control(dev, RT_DEVICE_CTRL_WDT_START, RT_NULL) == RT_EOK, "start");
    for (i = 0; i < 12; i++)        /* 6 s = three periods */
    {
        rt_thread_mdelay(500);
        rt_device_control(dev, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
    }
    TEST_CHECK(&c, RT_TRUE, "alive after three periods with keepalive");
    TEST_CHECK(&c, rt_device_control(dev, RT_DEVICE_CTRL_WDT_STOP, RT_NULL) == RT_EOK, "stop");
    rt_thread_mdelay(2500);
    TEST_CHECK(&c, RT_TRUE, "alive after stop (no reset)");
    rt_device_close(dev);
    return test_summary("wdt", &c);
}
MSH_CMD_EXPORT(test_wdt, watchdog keepalive and stop);

static int test_wdt_reset(int argc, char **argv)
{
    rt_device_t dev = rt_device_find("wdt0");
    rt_uint32_t to = 1;

    rt_device_open(dev, RT_DEVICE_OFLAG_RDWR);
    rt_device_control(dev, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &to);
    rt_device_control(dev, RT_DEVICE_CTRL_WDT_START, RT_NULL);
    rt_kprintf("watchdog started with 1 s, no feeding: the chip resets\n");
    for (;;)
        ;
}
MSH_CMD_EXPORT(test_wdt_reset, let the watchdog reset the chip);
