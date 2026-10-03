/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>

static void heartbeat(void *p)
{
    int i;

    for (i = 0; i < 5; i++)
    {
        rt_thread_mdelay(1000);
        rt_kprintf("heartbeat %d tick=%u\n", i, (rt_uint32_t)rt_tick_get());
    }
}

int main(void)
{
    rt_kprintf("Hello RT-Thread on the Allwinner sun252iw2\n");
    rt_thread_startup(rt_thread_create("hb", heartbeat, RT_NULL, 2048, 15, 5));
    return 0;
}
