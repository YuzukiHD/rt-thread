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

#endif
