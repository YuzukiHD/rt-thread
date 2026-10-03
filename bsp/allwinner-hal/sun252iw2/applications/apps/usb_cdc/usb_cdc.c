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
#include "cdc_device.h"
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
static const struct usb_cdc_hooks *hooks;

void usb_cdc_set_hooks(const struct usb_cdc_hooks *h)
{
    hooks = h;
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
    if (hooks && hooks->rx)
        hooks->rx(read_buffer, nbytes);
    usbd_ep_start_read(busid, CDC_OUT_EP, read_buffer, XFER_SIZE);
}

static void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    if (hooks && hooks->tx)
        hooks->tx(nbytes);
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

/* stream the 16 KiB buffer to the host for as long as the port is open (pattern: byte n is n & 0xff) */
void usb_cdc_source(rt_bool_t on, rt_bool_t pattern)
{
    if (!write_buffer)
        return;
    if (on && pattern)
    {
        rt_uint32_t n;

        for (n = 0; n < XFER_SIZE; n++)
            write_buffer[n] = n & 0xff;
    }
    src_on = on;
    start_source(0);
}
