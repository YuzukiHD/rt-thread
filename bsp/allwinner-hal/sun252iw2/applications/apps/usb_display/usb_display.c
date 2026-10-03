/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB second screen: the board enumerates as a virtual display for the Windows "usb graphic" display
 * driver, the PC sends its desktop as JPEG frames over a bulk endpoint, the video engine decodes
 * them and the picture goes to the video plane of the display.
 *
 *   usb_display_start [width height [fps [quality [frame limit KB]]]]
 *                           enumerate as a display of that size (default: the size of the panel; the video plane
 *                           of the display engine scales the picture to the panel, keeping its shape). The USB port
 *                           then stays a display of that size until reboot.
 *   usb_display_stop        stop showing frames
 *   usb_display_selftest    run a built-in JPEG through the decode and display path (no USB)
 */
#include <rtthread.h>
#include <rthw.h>
#include <string.h>
#include <stdlib.h>

#include "usbd_core.h"
#include "usbd_display.h"
#include "usb-glue-sun252i.h"
#include "vdec.h"
#include "drv_display.h"
#include "data/test_jpeg.h"
#include "usb_touch.h"

#define DISPLAY_IN_EP   0x81
#define DISPLAY_OUT_EP  0x02

#define USBD_VID        0x303A
#define USBD_PID        0x2987     /* the display alone */
#define USBD_PID_TOUCH  0x2986     /* composite: display on interface 0, touch screen on 1 */
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

/* CPU load: the idle thread counts while nothing else runs; the rate measured at the start is 0% load */
static volatile rt_uint32_t idle_count;
static rt_uint32_t idle_rate;   /* counts per ms of an idle system */

/*
 * The driver parses the product string: name, resolution, encoding (jpg quality 1..10), frame rate, buffer limit in KB.
 * The string is made by usb_display_start from its arguments.
 */
#define PRODUCT_FORMAT  "cherryusb_R%ux%u_Ejpg%u_Fps%u_Bl%u"
#ifndef ROUND_UP
#define ROUND_UP(x, a) (((x) + (a) - 1) / (a) * (a))
#endif
#define DEFAULT_FPS     60
#define DEFAULT_QUALITY 9
#define DEFAULT_BL_KB   500     /* the largest frame the buffers hold (FRAME_BUF_SIZE minus the headroom) */
#define MIN_SIZE        64
#define MAX_WIDTH       1920
#define MAX_HEIGHT      1080
static char product_string[64];
static rt_uint32_t host_w, host_h;     /* the size the host was told */

static rt_bool_t with_touch;
static uint8_t device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0101, 0x01)
};

/* filled by usb_display_start: the display interface, plus the touch screen if asked for */
static uint8_t config_descriptor[USB_CONFIG_SIZE + USB_TOUCH_DESCRIPTOR_SIZE];

static void build_config_descriptor(void)
{
    static const uint8_t display_part[] = {
        USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x00),
        USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_IN_EP, 0x02, DISPLAY_EP_MPS, 0x00),
        USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_OUT_EP, 0x02, DISPLAY_EP_MPS, 0x00),
    };
    uint8_t head[9] = { USB_CONFIG_DESCRIPTOR_INIT(0, 0x01, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER) };
    uint32_t len = sizeof(head);

    memcpy(config_descriptor, head, sizeof(head));
    memcpy(config_descriptor + len, display_part, sizeof(display_part));
    len += sizeof(display_part);
    if (with_touch)
    {
        len += usb_touch_descriptor(config_descriptor + len, USB_TOUCH_INTERFACE);
        config_descriptor[4] = 2;                               /* bNumInterfaces */
        device_descriptor[10] = USBD_PID_TOUCH & 0xFF;          /* idProduct */
        device_descriptor[11] = USBD_PID_TOUCH >> 8;
    }
    config_descriptor[2] = len & 0xFF;                          /* wTotalLength */
    config_descriptor[3] = len >> 8;
}

static const uint8_t device_quality_descriptor[] = {
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *string_descriptors[] = {
    (const char[]){ 0x09, 0x04 },   /* language id */
    "CherryUSB",                    /* manufacturer */
    product_string,                 /* product */
    "2022123456",                   /* serial number */
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
static int jpeg_hold = 3;           /* pictures the decoder keeps for us (1..3), chosen from the free memory */
#define JPEG_HOLD_MIN   1           /* 1: the show waits for the refresh and only one picture is kept; 2 or more: it does not wait, two are kept */
#define JPEG_VBV_SIZE   (1024 * 1024)
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
        cfg.buffer_size = JPEG_VBV_SIZE;
        cfg.holding_frames = jpeg_hold;
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
    /*
     * With room for two kept pictures the show does not wait for the refresh and the picture before the last is freed one
     * step late (it may still be scanned out). With one kept picture the show waits, then the picture before is free.
     */
    yuv.nonblock = jpeg_hold >= 2;
    ret = lcd_show_yuv(&yuv);
    show_us += now_us() - t0;
    if (jpeg_hold >= 2)
    {
        if (older.priv)
            vdec_frame_release(&older);
        older = shown;
    }
    else if (shown.priv)
    {
        vdec_frame_release(&shown);
    }
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
            const rt_uint8_t *d = frame->frame_buf + sizeof(*h);
            rt_uint32_t n = frame->frame_size, e = n;

            bytes_in += n;
            /* a whole JPEG: SOI first, EOI last (zero padding may follow it) */
            while (e > 2 && d[e - 1] == 0)
                e--;
            if (n < 4 || d[0] != 0xff || d[1] != 0xd8 || d[e - 2] != 0xff || d[e - 1] != 0xd9)
            {
                frames_bad++;
                if (frames_bad <= 12)
                {
                    rt_uint32_t k = n, nz = 0;

                    /* where the last EOI is, and how much non-zero data follows it */
                    while (k > 2 && !(d[k - 2] == 0xff && d[k - 1] == 0xd9))
                        k--;
                    for (e = k; e < n; e++)
                        nz += d[e] != 0;
                    rt_kprintf("usb display: broken frame %u: size %u, last EOI ends at %u (%u bytes before the end, %u non-zero after), size mod 512 %u, mod 16384 %u\n",
                               h->frame_id, n, k, n - k, nz, n & 511, n & 16383);
                }
            }
            else
            {
                int err = show_jpeg(frame->frame_buf + sizeof(*h), frame->frame_size, h->width, h->height);

                if (err == 0)
                {
                    frames_ok++;
                }
                else
                {
                    frames_bad++;
                    if (frames_bad <= 8)
                        rt_kprintf("usb display: frame %u (%ux%u, %u bytes) failed: %d\n", h->frame_id, h->width, h->height, n, err);
                }
            }
        }
        usbd_display_enqueue(frame);
report:
        if (now_us() - last >= REPORT_MS * 1000u)
        {
            rt_uint32_t ms = (now_us() - last) / 1000, n = frames_ok - start_ok;

            rt_uint32_t rate = idle_count / ms, load = rate >= idle_rate ? 0 : 100 - rate * 100 / idle_rate;

            idle_count = 0;
            rt_kprintf("usb display: %u frames (%u.%02u fps), %u KB/s, %u bad, decode %u us, show %u us per frame, CPU %u%%\n",
                       n, n * 1000 / ms, (n * 100000 / ms) % 100, (bytes_in - start_bytes) / ms, frames_bad,
                       n ? decode_us / n : 0, n ? show_us / n : 0, load);
            decode_us = show_us = 0;
            start_ok = frames_ok;
            start_bytes = bytes_in;
            last = now_us();
        }
    }
    stream_close();
    running = RT_FALSE;
}

static void idle_hook(void)
{
    idle_count++;
}

static int usb_display_start(int argc, char **argv)
{
    rt_thread_t t;
    rt_ubase_t base = sun252i_usb_otg_base();
    rt_uint32_t w = lcd_width(), h = lcd_height(), fps = DEFAULT_FPS, quality = DEFAULT_QUALITY, bl_kb = DEFAULT_BL_KB;
    int i;

    /* a trailing "touch" adds the touch screen interface */
    if (argc > 1 && !rt_strcmp(argv[argc - 1], "touch"))
    {
        with_touch = RT_TRUE;
        argc--;
    }
    if (argc > 1 && argc < 3)
    {
        rt_kprintf("usage: usb_display_start [width height [fps [quality 1..10 [frame limit KB]]]] [touch]\n");
        return -1;
    }
    if (argc > 2)
    {
        w = atoi(argv[1]);
        h = atoi(argv[2]);
    }
    if (argc > 3)
        fps = atoi(argv[3]);
    if (argc > 4)
        quality = atoi(argv[4]);
    if (argc > 5)
        bl_kb = atoi(argv[5]);
    if (w < MIN_SIZE || h < MIN_SIZE || w > MAX_WIDTH || h > MAX_HEIGHT || (w & 1) || (h & 1) || fps < 1 || fps > 120 ||
        quality < 1 || quality > 10 || bl_kb < 16 || bl_kb > DEFAULT_BL_KB)
    {
        rt_kprintf("usb display: %ux%u, %u fps, quality %u not possible (size %d..%d x %d..%d, even, fps 1..120, quality 1..10)\n",
                   w, h, fps, quality, MIN_SIZE, MAX_WIDTH, MIN_SIZE, MAX_HEIGHT);
        return -1;
    }
    if (usb_up && (w != host_w || h != host_h))
    {
        rt_kprintf("usb display: the host was told %ux%u; reboot to change the size\n", host_w, host_h);
        return -1;
    }

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
    if (!idle_rate)
    {
        rt_uint32_t t0 = rt_tick_get_millisecond();

        idle_count = 0;
        rt_thread_idle_sethook(idle_hook);
        rt_thread_mdelay(200);
        idle_rate = idle_count / (rt_tick_get_millisecond() - t0);
    }
    if (!usb_up)
    {
        rt_snprintf(product_string, sizeof(product_string), PRODUCT_FORMAT, w, h, quality, fps, bl_kb);
        host_w = w;
        host_h = h;
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
        {
            /*
             * The pictures of the decoder (two plus the held ones, 16 aligned NV12) and its stream buffer must fit what is
             * left. One held picture (blocking show) is the least that works: the picture on the screen, the one the engine
             * decodes into and the one waiting to be shown; two held pictures let the show run without waiting.
             */
            rt_size_t total, used, max_used;
            rt_uint64_t pic = (rt_uint64_t)ROUND_UP(w, 16) * ROUND_UP(h, 16) * 3 / 2;
            rt_uint64_t room;

            rt_memory_info(&total, &used, &max_used);
            room = total - used > JPEG_VBV_SIZE + 256 * 1024 ? total - used - JPEG_VBV_SIZE - 256 * 1024 : 0;
            for (jpeg_hold = 3; jpeg_hold >= JPEG_HOLD_MIN && pic * (2 + jpeg_hold) > room; jpeg_hold--)
                ;
            if (jpeg_hold < JPEG_HOLD_MIN)
            {
                rt_kprintf("usb display: %ux%u does not fit the memory: a picture takes %u KB, the decoder needs three and "
                           "%u KB are free (about %u pixels at most)\n", w, h, (rt_uint32_t)(pic / 1024), (rt_uint32_t)(room / 1024),
                           (rt_uint32_t)(room * 2 / 9));
                for (i = 0; i < FRAME_COUNT; i++)
                {
                    rt_free_align(frame_pool[i].frame_buf);
                    frame_pool[i].frame_buf = RT_NULL;
                }
                jpeg_hold = 3;
                return -1;
            }
            rt_kprintf("usb display: %ux%u, %u KB per picture, %d kept (%s show)\n", w, h, (rt_uint32_t)(pic / 1024), jpeg_hold,
                       jpeg_hold >= 2 ? "no-wait" : "waiting");
        }
        build_config_descriptor();
        usbd_desc_register(0, &display_descriptor);
        usbd_add_interface(0, usbd_display_init_intf(&display_intf, DISPLAY_OUT_EP, DISPLAY_IN_EP, frame_pool, FRAME_COUNT));
        if (with_touch)
        {
            usb_touch_init(0);
            usb_ctp_start();
        }
        usbd_initialize(0, base, usbd_event_handler);
        usb_up = RT_TRUE;
    }
    stop_req = RT_FALSE;
    frames_ok = frames_bad = bytes_in = decode_us = show_us = 0;
    t = rt_thread_create("usbdisp", display_thread, RT_NULL, 8192, 8, 10);
    if (!t)
        return -1;
    rt_thread_startup(t);
    rt_kprintf("usb display: waiting for the host (%s), shown on the %ux%u panel\n", product_string, lcd_width(), lcd_height());

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
