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

static int display_show_fb(const struct display_mode *mode)
{
    struct display_pipeline_state state;
    struct display_plane_state *p;

    display_pipeline_state_init(&state);
    state.plane_count = 0;
    p = &state.planes[state.plane_count++];
    memset(p, 0, sizeof(*p));
    p->enable = true;
    p->plane_id = FB_PLANE;
    p->alpha = 0xff;
    p->blend_mode = DISPLAY_BLEND_NONE;
    p->framebuffer.address = (uintptr_t)lcd_fb;
    p->framebuffer.plane_address[0] = (uintptr_t)lcd_fb;
    p->framebuffer.plane_stride[0] = mode->width * 4;
    p->framebuffer.plane_count = 1;
    p->framebuffer.format = DISPLAY_FORMAT_ARGB8888;
    p->framebuffer.width = mode->width;
    p->framebuffer.height = mode->height;
    p->framebuffer.stride = mode->width * 4;
    p->destination.width = mode->width;
    p->destination.height = mode->height;
    return display_submit(&state);
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
