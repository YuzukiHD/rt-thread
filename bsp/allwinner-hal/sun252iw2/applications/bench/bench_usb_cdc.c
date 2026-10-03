/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB device speed over the CDC ACM port: OUT and IN rates, the CPU load of the traffic and a byte pattern check of both
 * directions. usb_bench_start brings the port up; the host side is a script that writes or reads a pattern (byte n is
 * n & 0xff) as fast as it can. Needs BSP_USING_BENCH.
 */
#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include <usb/cherryusb/usb-glue-sun252i.h>

#include "cdc_device.h"

static volatile rt_uint32_t bytes_out, bytes_in;
static volatile rt_uint32_t cpu_idle_rate;      /* counts per ms with nothing going on */
static volatile rt_uint32_t cpu_counter;

/* the window of one measurement runs from the first to the last transfer of the traffic */
static volatile rt_uint32_t act_first_ms, act_last_ms, act_cpu_first, act_cpu_last, act_out_first, act_in_first;
static volatile rt_bool_t act_started;
static volatile rt_bool_t check_on = RT_TRUE;   /* verify the OUT stream: byte n of a burst is n & 0xff */
static volatile rt_uint32_t out_pos, out_errors, out_first_err;

static void note_activity(void)
{
    rt_uint32_t now = rt_tick_get_millisecond();

    if (!act_started)
    {
        act_started = RT_TRUE;
        act_first_ms = now;
        act_cpu_first = cpu_counter;
        act_out_first = bytes_out;
        act_in_first = bytes_in;
        out_pos = 0;
        out_errors = 0;
    }
    act_last_ms = now;
    act_cpu_last = cpu_counter;
}

static void bench_rx(const uint8_t *data, uint32_t nbytes)
{
    bytes_out += nbytes;
    note_activity();
    if (check_on)
    {
        rt_uint32_t n;

        for (n = 0; n < nbytes; n++)
        {
            if (data[n] != ((out_pos + n) & 0xff) && out_errors++ == 0)
                out_first_err = out_pos + n;
        }
        out_pos += nbytes;
    }
}

static void bench_tx(uint32_t nbytes)
{
    bytes_in += nbytes;
    note_activity();
}

static const struct usb_cdc_hooks bench_hooks = { .rx = bench_rx, .tx = bench_tx };

/* bring the CDC port up and let the host see a fresh attach */
static int usb_bench_start(int argc, char **argv)
{
    rt_ubase_t base = sun252i_usb_otg_base();
    volatile rt_uint8_t *power = (volatile rt_uint8_t *)(base + 0x40);

    if (!base)
    {
        rt_kprintf("usb bench: no OTG controller in the device tree\n");
        return -1;
    }
    usb_cdc_set_hooks(&bench_hooks);
    cdc_acm_init(0, base);
    rt_thread_mdelay(200);
    *power &= ~0x40u;
    rt_thread_mdelay(500);
    *power |= 0x40u;

    return 0;
}
MSH_CMD_EXPORT(usb_bench_start, start the CDC ACM device for the USB benchmark);

/* the idle hook counts how often the idle thread runs: what it counts is the CPU time left over */
static void cpu_hook(void)
{
    cpu_counter++;
}

static void cpu_start(void)
{
    static rt_bool_t started;

    if (!started)
    {
        rt_thread_idle_sethook(cpu_hook);
        started = RT_TRUE;
    }
}

static int usb_bench_cpu(int argc, char **argv)
{
    rt_uint32_t c0;

    cpu_start();
    rt_thread_mdelay(100);
    c0 = cpu_counter;
    rt_thread_mdelay(1000);
    cpu_idle_rate = (cpu_counter - c0) / 1000;
    rt_kprintf("usb bench: idle %u counts/ms\n", cpu_idle_rate);

    return 0;
}
MSH_CMD_EXPORT(usb_bench_cpu, measure the idle CPU rate for usb_bench_stat);

static int usb_bench_src(int argc, char **argv)
{
    rt_bool_t on = argc > 1 && argv[1][0] != '0';

    /* byte n of the stream is n & 0xff (a transfer is a multiple of 256 bytes) */
    usb_cdc_source(on, RT_TRUE);
    rt_kprintf("usb bench: source %s\n", on ? "on" : "off");

    return 0;
}
MSH_CMD_EXPORT(usb_bench_src, stream data to the host: usb_bench_src 1|0);

static int usb_bench_stat(int argc, char **argv)
{
    rt_uint32_t ms, o, i, c, load = 0;

    if (!act_started)
    {
        rt_kprintf("usb bench: no traffic since the last call\n");
        return 0;
    }
    ms = act_last_ms - act_first_ms;
    o = bytes_out - act_out_first;
    i = bytes_in - act_in_first;
    c = act_cpu_last - act_cpu_first;
    if (ms == 0)
        ms = 1;
    if (cpu_idle_rate)
    {
        rt_uint32_t rate = c / ms;

        load = rate >= cpu_idle_rate ? 0 : 100 - rate * 100 / cpu_idle_rate;
    }
    rt_kprintf("usb bench: OUT %u KB/s, IN %u KB/s over %u ms of traffic, CPU load %u%%\n", o / ms, i / ms, ms, load);
    if (check_on && o)
        rt_kprintf("usb bench: OUT check %u bytes, %u errors (first at %u)\n", out_pos, out_errors, out_first_err);
    act_started = RT_FALSE;

    return 0;
}
static int usb_bench_check(int argc, char **argv)
{
    check_on = argc > 1 && argv[1][0] != '0';
    rt_kprintf("usb bench: OUT check %s\n", check_on ? "on" : "off");

    return 0;
}
MSH_CMD_EXPORT(usb_bench_check, verify the OUT stream pattern: usb_bench_check 1|0);

MSH_CMD_EXPORT(usb_bench_stat, USB rates over the last burst of traffic and the CPU load);

/* background memory traffic: copies through the CPU cache, so that dirty lines are evicted all over RAM */
static volatile rt_bool_t load_on;

static void load_thread(void *arg)
{
    const rt_size_t size = 1024 * 1024;
    rt_uint8_t *a = rt_malloc_align(size, 64), *b = rt_malloc_align(size, 64);

    while (a && b && load_on)
    {
        memcpy(b, a, size);
        memset(a, 0x5a, size);
        rt_thread_mdelay(1);
    }
    rt_free_align(a);
    rt_free_align(b);
}

static int usb_bench_load(int argc, char **argv)
{
    load_on = argc > 1 && argv[1][0] != '0';
    if (load_on)
        rt_thread_startup(rt_thread_create("usbload", load_thread, RT_NULL, 2048, 25, 5));
    rt_kprintf("usb bench: memory load %s\n", load_on ? "on" : "off");

    return 0;
}
MSH_CMD_EXPORT(usb_bench_load, memory traffic while benchmarking: usb_bench_load 1|0);
