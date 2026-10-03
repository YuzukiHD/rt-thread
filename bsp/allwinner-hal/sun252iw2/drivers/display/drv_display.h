/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display engine + TCON + panel with the framebuffer of the UI channel.
 */
#ifndef __DRV_DISPLAY_H__
#define __DRV_DISPLAY_H__

#include <rtthread.h>

/* the framebuffer is ARGB8888 (0xAARRGGBB, one word per pixel), NULL until the pipeline runs */
rt_uint32_t *lcd_framebuffer(void);
rt_uint32_t lcd_width(void);
rt_uint32_t lcd_height(void);
void lcd_flush(void);

/* a NV12 picture on the video plane, scaled to the screen; the UI plane stays above it */
struct lcd_yuv
{
    const void *y;
    const void *uv;
    rt_uint32_t width, height;
    rt_uint32_t stride_y, stride_uv;
    rt_bool_t bt709;
    rt_bool_t nonblock;     /* up at the next refresh, do not wait for it */
};

int lcd_show_yuv(const struct lcd_yuv *img);
int lcd_hide_yuv(void);

#endif
