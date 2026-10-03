/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "drv_display.h"

static void heartbeat(void *p)
{
    int i;

    for (i = 0; i < 5; i++)
    {
        rt_thread_mdelay(1000);
        rt_kprintf("heartbeat %d tick=%u\n", i, (rt_uint32_t)rt_tick_get());
    }
}

/* the boot picture: eight colour bars over a gray gradient, then the backlight */
static void boot_screen(void)
{
    static const rt_uint32_t bars[8] = {0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
                                        0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000};
    rt_uint32_t *fb = lcd_framebuffer();
    rt_uint32_t w = lcd_width(), h = lcd_height(), x, y;
    struct rt_device_pwm *bl = (struct rt_device_pwm *)rt_device_find("pwm_bl0");

    if (!fb)
        return;
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            rt_uint32_t g = x * 255 / (w - 1);

            fb[y * w + x] = y < h * 2 / 3 ? bars[x * 8 / w] : (0xff000000u | (g << 16) | (g << 8) | g);
        }
    }
    lcd_flush();
    if (bl)
    {
        rt_pwm_set(bl, 0, 1000000, 600000);
        rt_pwm_enable(bl, 0);
    }
}

int main(void)
{
    rt_kprintf("Hello RT-Thread on the Allwinner sun252iw2\n");
    boot_screen();
    rt_thread_startup(rt_thread_create("hb", heartbeat, RT_NULL, 2048, 15, 5));
    return 0;
}
