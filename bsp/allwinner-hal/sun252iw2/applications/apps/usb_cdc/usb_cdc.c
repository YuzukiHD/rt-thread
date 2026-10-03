/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB CDC ACM device (a COM port on the host) with a throughput benchmark. The host side of the
 * benchmark is any program that writes to the port (device receives, "OUT") or reads from it
 * (device sends, "IN", started with usb_bench_src 1 once the host has opened the port).
 *
 *   usb_bench_cpu      measure the idle CPU rate (run with no traffic first)
 *   usb_bench_src 1|0  stream data to the host on the IN endpoint (needs DTR)
 *   usb_bench_stat     print the rates since the last call and the CPU load
 */
#include <rtthread.h>
#include <rthw.h>
#include <string.h>

#include "usbd_core.h"
#include "usbd_cdc_acm.h"

#define CDC_IN_EP       0x81
#define CDC_OUT_EP      0x02
#define CDC_INT_EP      0x83

#define USBD_VID        0xFFFF
#define USBD_PID        0xFFFF
#define USBD_MAX_POWER  100

#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)

#ifdef CONFIG_USB_HS
#define CDC_MAX_MPS     512
#else
#define CDC_MAX_MPS     64
#endif

#define XFER_SIZE       16384

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02)
};

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },   /* language id */
    "RT-Thread",                    /* manufacturer */
    "sun252iw2 CDC",                /* product */
    "sun252iw2-cdc",                /* serial number */
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
    return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    if (index >= (sizeof(string_descriptors) / sizeof(char *)))
        return NULL;

    return string_descriptors[index];
}

static const struct usb_descriptor cdc_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback
};

static uint8_t *read_buffer, *write_buffer;
static volatile rt_bool_t tx_busy, dtr_enable, src_on;
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

static void start_source(uint8_t busid)
{
    if (src_on && dtr_enable && !tx_busy)
    {
        tx_busy = RT_TRUE;
        if (usbd_ep_start_write(busid, CDC_IN_EP, write_buffer, XFER_SIZE) != 0)
            tx_busy = RT_FALSE;
    }
}

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    if (event == USBD_EVENT_CONFIGURED)
    {
        tx_busy = RT_FALSE;
        /* setup the first OUT transfer */
        usbd_ep_start_read(busid, CDC_OUT_EP, read_buffer, XFER_SIZE);
    }
}

static void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    bytes_out += nbytes;
    note_activity();
    if (check_on)
    {
        rt_uint32_t n;

        for (n = 0; n < nbytes; n++)
        {
            if (read_buffer[n] != ((out_pos + n) & 0xff) && out_errors++ == 0)
                out_first_err = out_pos + n;
        }
        out_pos += nbytes;
    }
    usbd_ep_start_read(busid, CDC_OUT_EP, read_buffer, XFER_SIZE);
}

static void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    bytes_in += nbytes;
    note_activity();
    tx_busy = RT_FALSE;
    start_source(busid);
}

static struct usbd_endpoint cdc_out_ep = {
    .ep_addr = CDC_OUT_EP,
    .ep_cb = usbd_cdc_acm_bulk_out
};

static struct usbd_endpoint cdc_in_ep = {
    .ep_addr = CDC_IN_EP,
    .ep_cb = usbd_cdc_acm_bulk_in
};

static struct usbd_interface intf0, intf1;

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    dtr_enable = dtr;
    start_source(busid);
}

void cdc_acm_init(uint8_t busid, uintptr_t reg_base)
{
    const uint8_t data[10] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30 };

    read_buffer = rt_malloc_align(XFER_SIZE, 64);
    write_buffer = rt_malloc_align(XFER_SIZE, 64);
    if (!read_buffer || !write_buffer)
    {
        rt_kprintf("usb cdc: no memory\n");
        return;
    }
    memcpy(write_buffer, data, 10);
    memset(write_buffer + 10, 'a', XFER_SIZE - 10);

    usbd_desc_register(busid, &cdc_descriptor);
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &intf0));
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &intf1));
    usbd_add_endpoint(busid, &cdc_out_ep);
    usbd_add_endpoint(busid, &cdc_in_ep);
    usbd_initialize(busid, reg_base, usbd_event_handler);
}

/* send a 2048 byte pattern to the host once (needs DTR, i.e. an open port) */
void cdc_acm_data_send_with_dtr_test(uint8_t busid)
{
    if (dtr_enable && !tx_busy)
    {
        tx_busy = RT_TRUE;
        usbd_ep_start_write(busid, CDC_IN_EP, write_buffer, 2048);
        while (tx_busy)
            rt_thread_mdelay(1);
    }
}

/* ---- benchmark ------------------------------------------------------------------------- */

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
    src_on = argc > 1 && argv[1][0] != '0';
    if (src_on)
    {
        rt_uint32_t n;

        /* byte n of the stream is n & 0xff (a transfer is a multiple of 256 bytes) */
        for (n = 0; n < XFER_SIZE; n++)
            write_buffer[n] = n & 0xff;
    }
    start_source(0);
    rt_kprintf("usb bench: source %s\n", src_on ? "on" : "off");

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
