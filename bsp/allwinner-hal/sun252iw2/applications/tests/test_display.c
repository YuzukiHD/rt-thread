/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display: registers of the running pipeline (TCON enable, DE UI0 address)
 * and a test picture: eight colour bars, a white frame and a gradient. The
 * picture has to be looked at, nothing here can see the panel.
 */
#include <rtthread.h>
#include "drv_display.h"
#include "test.h"

static int test_display(int argc, char **argv)
{
    struct test_ctx c = {0, 0};
    rt_uint32_t *fb = lcd_framebuffer();
    rt_uint32_t w = lcd_width(), h = lcd_height();
    volatile rt_uint32_t *tcon = test_node_regs("/soc/tcon-lcd@5461000");
    volatile rt_uint32_t *de = test_node_regs("/soc/display-engine@5000000");
    struct rt_device_pwm *bl = (struct rt_device_pwm *)rt_device_find("pwm_bl0");
    rt_uint32_t x, y;
    static const rt_uint32_t bars[8] = {0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
                                        0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000};

    TEST_CHECK(&c, fb != RT_NULL, "framebuffer allocated");
    if (!fb) return test_summary("display", &c);

    rt_kprintf("lcd: %ux%u, TCON ctl %08x, DE UI0 attr %08x laddr %08x\n", w, h,
               tcon ? tcon[0] : 0u, de ? de[0x161000 / 4] : 0u, de ? de[0x161010 / 4] : 0u);
    TEST_CHECK(&c, tcon && (tcon[0] & 0x80000000u) != 0u, "TCON LCD enable bit set");
    TEST_CHECK(&c, de && de[0x161010 / 4] == (rt_uint32_t)fb, "DE UI0 reads the framebuffer address");

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            rt_uint32_t v;
            if (y < h * 2 / 3)
                v = bars[x * 8 / w];
            else
            {
                rt_uint32_t g = x * 255 / (w - 1);
                v = 0xff000000u | (g << 16) | (g << 8) | g;
            }
            if (x < 4 || y < 4 || x >= w - 4 || y >= h - 4)
                v = 0xffffffffu;
            fb[y * w + x] = v;
        }
    lcd_flush();
    if (bl)
    {
        rt_pwm_set(bl, 0, 1000000, 600000);
        rt_pwm_enable(bl, 0);
    }
    rt_kprintf("lcd: test picture drawn (8 bars, gradient, white frame) - look at the panel\n");
    return test_summary("display", &c);
}
MSH_CMD_EXPORT(test_display, draw colour bars on the panel);
