/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 *
 * Framebuffer front end of the display stack: one UI plane of the display
 * engine scans out an ARGB8888 framebuffer in PSRAM.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>

#include <hal/display/display_engine.h>

#include "dt_graph.h"
#include "drv_display.h"

/* UI channel 0, layer 0 */
#define FB_PLANE        4

static rt_uint32_t *lcd_fb;
static rt_uint32_t lcd_w, lcd_h;

rt_uint32_t *lcd_framebuffer(void)
{
    return lcd_fb;
}

rt_uint32_t lcd_width(void)
{
    return lcd_w;
}

rt_uint32_t lcd_height(void)
{
    return lcd_h;
}

void lcd_flush(void)
{
    if (lcd_fb)
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, lcd_fb, lcd_w * lcd_h * 4);
}

/* the UI plane over the whole screen; with @blend a coverage blend the transparent parts show the video */
static void fb_plane(struct display_plane_state *p, rt_bool_t enable, uint8_t blend)
{
    memset(p, 0, sizeof(*p));
    p->enable = enable;
    p->plane_id = FB_PLANE;
    p->alpha = 0xff;
    p->blend_mode = blend;
    p->framebuffer.address = (uintptr_t)lcd_fb;
    p->framebuffer.plane_address[0] = (uintptr_t)lcd_fb;
    p->framebuffer.plane_stride[0] = lcd_w * 4;
    p->framebuffer.plane_count = 1;
    p->framebuffer.format = DISPLAY_FORMAT_ARGB8888;
    p->framebuffer.width = lcd_w;
    p->framebuffer.height = lcd_h;
    p->framebuffer.stride = lcd_w * 4;
    p->destination.width = lcd_w;
    p->destination.height = lcd_h;
}

static int display_show_fb(const struct display_mode *mode)
{
    struct display_pipeline_state state;

    display_pipeline_state_init(&state);
    state.plane_count = 1;
    fb_plane(&state.planes[0], RT_TRUE, DISPLAY_BLEND_NONE);

    return display_submit(&state);
}

/* the first plane that takes YCbCr and scales; it is not the frame buffer plane */
static int find_video_plane(void)
{
    struct display_caps caps;
    struct display_plane_caps pc;
    uint32_t id;

    if (display_get_caps(&caps) != 0)
        return -1;
    for (id = 0; id < caps.plane_count; id++)
    {
        if (id == FB_PLANE || display_get_plane_caps(id, &pc) != 0)
            continue;
        if ((pc.flags & DISPLAY_PLANE_CAP_YUV) && (pc.flags & DISPLAY_PLANE_CAP_SCALE))
            return id;
    }

    return -1;
}

int lcd_show_yuv(const struct lcd_yuv *img)
{
    struct display_pipeline_state state;
    struct display_plane_state *v;
    static int video_plane = -2;
    uint32_t dw, dh;

    if (!lcd_fb)
        return -RT_ENOSYS;
    if (video_plane == -2)
        video_plane = find_video_plane();
    if (video_plane < 0)
        return -RT_ENOSYS;

    /* largest size that fits the screen and keeps the shape of the picture */
    if ((uint64_t)lcd_w * img->height <= (uint64_t)lcd_h * img->width)
    {
        dw = lcd_w;
        dh = (uint64_t)img->height * lcd_w / img->width;
    }
    else
    {
        dh = lcd_h;
        dw = (uint64_t)img->width * lcd_h / img->height;
    }
    dw &= ~1U;
    dh &= ~1U;

    display_pipeline_state_init(&state);
    state.plane_count = 2;
    /* the UI plane stays on above the video: what is drawn on it is blended over the picture */
    fb_plane(&state.planes[0], RT_TRUE, DISPLAY_BLEND_COVERAGE);
    v = &state.planes[1];
    memset(v, 0, sizeof(*v));
    v->enable = true;
    v->plane_id = video_plane;
    v->alpha = 0xff;
    v->blend_mode = DISPLAY_BLEND_NONE;
    v->color_encoding = img->bt709 ? DISPLAY_COLOR_BT709 : DISPLAY_COLOR_BT601;
    v->color_range = DISPLAY_RANGE_LIMITED;
    v->framebuffer.address = (uintptr_t)img->y;
    v->framebuffer.plane_address[0] = (uintptr_t)img->y;
    v->framebuffer.plane_address[1] = (uintptr_t)img->uv;
    v->framebuffer.plane_stride[0] = img->stride_y;
    v->framebuffer.plane_stride[1] = img->stride_uv;
    v->framebuffer.plane_count = 2;
    v->framebuffer.format = DISPLAY_FORMAT_NV12;
    v->framebuffer.width = img->width;
    v->framebuffer.height = img->height;
    v->framebuffer.stride = img->stride_y;
    v->source.width = img->width;
    v->source.height = img->height;
    v->destination.x = (lcd_w - dw) / 2;
    v->destination.y = (lcd_h - dh) / 2;
    v->destination.width = dw;
    v->destination.height = dh;

    return display_submit_ex(&state, DISPLAY_SUBMIT_PARTIAL | (img->nonblock ? DISPLAY_SUBMIT_NONBLOCK : 0));
}

int lcd_show_rgb(const struct lcd_rgb *img)
{
    struct display_pipeline_state state;
    struct display_plane_state *v;
    static int video_plane = -2;
    uint32_t dw, dh;

    if (!lcd_fb)
        return -RT_ENOSYS;
    if (video_plane == -2)
        video_plane = find_video_plane();
    if (video_plane < 0)
        return -RT_ENOSYS;

    if ((uint64_t)lcd_w * img->height <= (uint64_t)lcd_h * img->width)
    {
        dw = lcd_w;
        dh = (uint64_t)img->height * lcd_w / img->width;
    }
    else
    {
        dh = lcd_h;
        dw = (uint64_t)img->width * lcd_h / img->height;
    }
    dw &= ~1U;
    dh &= ~1U;

    display_pipeline_state_init(&state);
    state.plane_count = 2;
    fb_plane(&state.planes[0], RT_TRUE, DISPLAY_BLEND_COVERAGE);
    v = &state.planes[1];
    memset(v, 0, sizeof(*v));
    v->enable = true;
    v->plane_id = video_plane;
    v->alpha = 0xff;
    v->blend_mode = DISPLAY_BLEND_NONE;
    v->framebuffer.address = (uintptr_t)img->data;
    v->framebuffer.plane_address[0] = (uintptr_t)img->data;
    v->framebuffer.plane_stride[0] = img->stride;
    v->framebuffer.plane_count = 1;
    v->framebuffer.format = img->xrgb8888 ? DISPLAY_FORMAT_XRGB8888 : DISPLAY_FORMAT_RGB565;
    v->framebuffer.width = img->width;
    v->framebuffer.height = img->height;
    v->framebuffer.stride = img->stride;
    v->source.width = img->width;
    v->source.height = img->height;
    v->destination.x = (lcd_w - dw) / 2;
    v->destination.y = (lcd_h - dh) / 2;
    v->destination.width = dw;
    v->destination.height = dh;

    return display_submit_ex(&state, DISPLAY_SUBMIT_PARTIAL | (img->nonblock ? DISPLAY_SUBMIT_NONBLOCK : 0));
}

int lcd_hide_yuv(void)
{
    struct display_pipeline_state state;

    if (!lcd_fb)
        return -RT_ENOSYS;
    display_pipeline_state_init(&state);
    state.plane_count = 1;
    fb_plane(&state.planes[0], RT_TRUE, DISPLAY_BLEND_NONE);

    return display_submit_ex(&state, DISPLAY_SUBMIT_PARTIAL);
}

static void display_thread(void *param)
{
    struct display_mode mode;
    int ret;

    ret = dpy_ofw_prepare();
    if (ret)
    {
        rt_kprintf("lcd: no display pipeline in the device tree: %d\n", ret);
        return;
    }
    ret = display_probe();
    if (ret)
    {
        rt_kprintf("lcd: display_probe failed: %d\n", ret);
        return;
    }
    ret = display_wait_ready(5000);
    if (ret)
    {
        rt_kprintf("lcd: display not ready: %d\n", ret);
        return;
    }
    ret = display_get_mode(&mode);
    if (ret)
    {
        rt_kprintf("lcd: no display mode: %d\n", ret);
        return;
    }
    lcd_w = mode.width;
    lcd_h = mode.height;
    lcd_fb = rt_malloc_align(lcd_w * lcd_h * 4, 64);
    if (lcd_fb == RT_NULL)
    {
        rt_kprintf("lcd: no memory for the framebuffer\n");
        return;
    }
    memset(lcd_fb, 0, lcd_w * lcd_h * 4);
    lcd_flush();
    ret = display_show_fb(&mode);
    if (ret)
    {
        rt_kprintf("lcd: cannot show the framebuffer: %d\n", ret);
        return;
    }
    rt_kprintf("lcd: %ux%u @ %u Hz, framebuffer at %p\n", mode.width, mode.height, mode.refresh_hz, lcd_fb);
}

/* the pipeline probes in a thread of its own: it sleeps and waits for the first frame */
static rt_err_t display_probe_dev(struct rt_platform_device *pdev)
{
    rt_thread_t tid = rt_thread_create("display", display_thread, RT_NULL, 4096, 10, 10);

    if (!tid)
        return -RT_ENOMEM;

    return rt_thread_startup(tid);
}

static const struct rt_ofw_node_id display_ofw_ids[] =
{
    { .compatible = "allwinner,sunxi-display" },
    { /* sentinel */ }
};

static struct rt_platform_driver display_driver =
{
    .name = "display-sunxi",
    .ids = display_ofw_ids,
    .probe = display_probe_dev,
};
RT_PLATFORM_DRIVER_EXPORT(display_driver);
