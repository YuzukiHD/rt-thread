/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB second screen: the board enumerates as a virtual display for the Windows "usb graphic" display
 * driver, the PC sends its desktop as JPEG frames over a bulk endpoint, the video engine decodes
 * them and the picture goes to the video plane of the display.
 *
 *   usb_display_start       enumerate as a display (the USB port then stays a display until reboot)
 *   usb_display_stop        stop showing frames
 *   usb_display_selftest    run a built-in JPEG through the decode and display path (no USB)
 */
#include <rtthread.h>
#include <rthw.h>
#include <string.h>

#include "usbd_core.h"
#include "usbd_display.h"
#include "usb-glue-sun252i.h"
#include "vdec.h"
#include "drv_display.h"
#include "data/test_jpeg.h"

#define DISPLAY_IN_EP   0x81
#define DISPLAY_OUT_EP  0x02

#define USBD_VID        0x303A
#define USBD_PID        0x2987
#define USBD_MAX_POWER  100

#define USB_CONFIG_SIZE (9 + 9 + 7 + 7)

#ifdef CONFIG_USB_HS
#define DISPLAY_EP_MPS  512
#else
#define DISPLAY_EP_MPS  64
#endif

/* a JPEG frame is at most 512 KiB; the driver reads in steps of 16 KiB past the end, and the size must not be a multiple of 16 KiB */
#define FRAME_BUF_SIZE  (512 * 1024 + 16 * 1024 + 4096)
#define FRAME_COUNT     2
#define REPORT_MS       5000

/* the driver parses the product string: name, resolution, encoding (jpg quality 1..10), frame rate, buffer limit in KB */
#define PRODUCT_STRING  "sun252iw2_R1024x600_Ejpg6_Fps30_Bl500"

static const uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0101, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_IN_EP, 0x02, DISPLAY_EP_MPS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_OUT_EP, 0x02, DISPLAY_EP_MPS, 0x00),
};

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },   /* language id */
    "RT-Thread",                    /* manufacturer */
    PRODUCT_STRING,                 /* product */
    "sun252iw2-0001",               /* serial number */
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

static const struct usb_descriptor display_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback
};

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    switch (event)
    {
    case USBD_EVENT_CONFIGURED:
        rt_kprintf("usb display: configured by the host\n");
        break;
    case USBD_EVENT_DISCONNECTED:
        rt_kprintf("usb display: disconnected\n");
        break;
    default:
        break;
    }
}

static struct usbd_interface display_intf;
static struct usbd_display_frame frame_pool[FRAME_COUNT];
static rt_bool_t usb_up;
static volatile rt_bool_t running, stop_req;

/*
 * A decoder that stays open while the picture size stays the same: motion JPEG through the stream
 * API. The decoded pictures go to the display as they are; the one before the last may still be
 * scanned out, so it is released one frame late.
 */
static struct vdec_stream *jpeg_stream;
static rt_uint32_t jpeg_w, jpeg_h;
static struct vdec_frame shown, older;

static rt_uint32_t frames_ok, frames_bad, bytes_in, decode_us, show_us;

static rt_uint32_t now_us(void)
{
    return rt_tick_get_millisecond() * 1000u;
}

static void stream_close(void)
{
    if (older.priv)
        vdec_frame_release(&older);
    if (shown.priv)
        vdec_frame_release(&shown);
    if (jpeg_stream)
        vdec_stream_close(jpeg_stream);
    jpeg_stream = RT_NULL;
}

/* decode one JPEG of w x h pixels and put it on the screen; 0 on success */
static int show_jpeg(const void *jpeg, size_t len, rt_uint32_t w, rt_uint32_t h)
{
    struct vdec_frame f;
    struct lcd_yuv yuv;
    size_t used;
    rt_uint32_t t0 = now_us();
    int ret, tries;

    if (jpeg_stream && (w != jpeg_w || h != jpeg_h))
        stream_close();
    if (!jpeg_stream)
    {
        struct vdec_stream_config cfg;

        memset(&cfg, 0, sizeof(cfg));
        cfg.codec = VDEC_CODEC_JPEG;
        cfg.format = VDEC_FORMAT_NV12;
        cfg.buffer_size = 1024 * 1024;
        cfg.width = w;
        cfg.height = h;
        ret = vdec_stream_open(&cfg, &jpeg_stream);
        if (ret)
            return ret;
        jpeg_w = w;
        jpeg_h = h;
    }
    ret = vdec_stream_feed(jpeg_stream, jpeg, len, 0, &used);
    if (ret)
        return ret;
    for (tries = 0; tries < 20; tries++)
    {
        ret = vdec_stream_get_frame(jpeg_stream, &f);
        if (ret != -EBUSY && ret != -EAGAIN)
            break;
        rt_thread_mdelay(1);
    }
    if (ret)
        return ret;
    decode_us += now_us() - t0;

    t0 = now_us();
    yuv.y = f.plane[0];
    yuv.uv = f.plane[1];
    yuv.width = f.width;
    yuv.height = f.height;
    yuv.stride_y = f.stride[0];
    yuv.stride_uv = f.stride[1];
    yuv.bt709 = RT_FALSE;
    yuv.nonblock = RT_TRUE;
    ret = lcd_show_yuv(&yuv);
    show_us += now_us() - t0;
    if (older.priv)
        vdec_frame_release(&older);
    older = shown;
    shown = f;

    return ret;
}

static void display_thread(void *arg)
{
    rt_uint32_t last = now_us(), start_ok = 0, start_bytes = 0;

    running = RT_TRUE;
    while (!stop_req)
    {
        struct usbd_display_frame *frame;
        struct usbd_disp_frame_header *h;

        if (usbd_display_dequeue(&frame, 1000) < 0)
            goto report;
        h = (struct usbd_disp_frame_header *)frame->frame_buf;
        /* a frame that fills the last packet exactly is followed by an empty frame of type 0xff */
        if (frame->frame_format == USBD_DISPLAY_TYPE_JPG && frame->frame_size > 0)
        {
            bytes_in += frame->frame_size;
            if (show_jpeg(frame->frame_buf + sizeof(*h), frame->frame_size, h->width, h->height) == 0)
                frames_ok++;
            else
                frames_bad++;
        }
        usbd_display_enqueue(frame);
report:
        if (now_us() - last >= REPORT_MS * 1000u)
        {
            rt_uint32_t ms = (now_us() - last) / 1000, n = frames_ok - start_ok;

            rt_kprintf("usb display: %u frames (%u.%02u fps), %u KB/s, %u bad, decode %u us, show %u us per frame\n",
                       n, n * 1000 / ms, (n * 100000 / ms) % 100, (bytes_in - start_bytes) / ms, frames_bad,
                       n ? decode_us / n : 0, n ? show_us / n : 0);
            decode_us = show_us = 0;
            start_ok = frames_ok;
            start_bytes = bytes_in;
            last = now_us();
        }
    }
    stream_close();
    running = RT_FALSE;
}

static int usb_display_start(int argc, char **argv)
{
    rt_thread_t t;
    rt_ubase_t base = sun252i_usb_otg_base();
    int i;

    if (!base)
    {
        rt_kprintf("usb display: no OTG controller in the device tree\n");
        return -1;
    }
    if (!lcd_framebuffer())
    {
        rt_kprintf("usb display: the display is not ready\n");
        return -1;
    }
    if (running)
    {
        rt_kprintf("usb display: already running\n");
        return -1;
    }
    if (!usb_up)
    {
        for (i = 0; i < FRAME_COUNT; i++)
        {
            frame_pool[i].frame_buf = rt_malloc_align(FRAME_BUF_SIZE, 64);
            frame_pool[i].frame_bufsize = FRAME_BUF_SIZE;
            if (!frame_pool[i].frame_buf)
            {
                rt_kprintf("usb display: no memory for the frame buffers\n");
                return -1;
            }
        }
        usbd_desc_register(0, &display_descriptor);
        usbd_add_interface(0, usbd_display_init_intf(&display_intf, DISPLAY_OUT_EP, DISPLAY_IN_EP, frame_pool, FRAME_COUNT));
        usbd_initialize(0, base, usbd_event_handler);
        usb_up = RT_TRUE;
    }
    stop_req = RT_FALSE;
    frames_ok = frames_bad = bytes_in = decode_us = show_us = 0;
    t = rt_thread_create("usbdisp", display_thread, RT_NULL, 8192, 8, 10);
    if (!t)
        return -1;
    rt_thread_startup(t);
    rt_kprintf("usb display: waiting for the host (%s)\n", PRODUCT_STRING);

    return 0;
}
MSH_CMD_EXPORT(usb_display_start, enumerate as a USB display and show the frames of the host);

static int usb_display_stop(int argc, char **argv)
{
    stop_req = RT_TRUE;
    while (running)
        rt_thread_mdelay(50);
    lcd_hide_yuv();

    return 0;
}
MSH_CMD_EXPORT(usb_display_stop, stop showing the frames of the USB display);

/* the decode and display path without USB: the built-in test picture, a few frames timed */
static int usb_display_selftest(int argc, char **argv)
{
    rt_uint32_t t0, i;
    int ret = 0;

    if (!lcd_framebuffer())
    {
        rt_kprintf("usb display: the display is not ready\n");
        return -1;
    }
    frames_ok = frames_bad = decode_us = show_us = 0;
    t0 = now_us();
    for (i = 0; i < 30 && ret == 0; i++)
        ret = show_jpeg(test_jpeg, sizeof(test_jpeg), 320, 240);
    stream_close();
    rt_kprintf("usb display: selftest %d, %u frames of 320x240 in %u ms (decode %u us, show %u us per frame)\n", ret, i,
               (now_us() - t0) / 1000, decode_us / (i ? i : 1), show_us / (i ? i : 1));

    return ret;
}
MSH_CMD_EXPORT(usb_display_selftest, decode and show the built-in JPEG through the USB display path);
