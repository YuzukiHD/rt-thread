/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board bring-up of the Allwinner sun252iw2 (C907, machine mode).
 *
 * The image is loaded into the PSRAM at 0x40000000 by the first-stage loader
 * (or the FEL tool together with the loader), which has already started the
 * console UART and the PSRAM. Everything else is described by the device tree
 * linked into the image and brought up by the device model.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "board.h"
#include "drv_systick.h"

/* CLINT: mtime at 0x1400bff8, 24 MHz */
#define CLINT_MTIME 0x1400BFF8u
#define TIMEBASE    24000000u

/* the blob of builtin_fdt.S */
extern const char __dtb_start[];

static volatile unsigned long *mtime = (volatile unsigned long *)CLINT_MTIME;

void rt_hw_us_delay(rt_uint32_t us)
{
    unsigned long start = *mtime;
    while ((*mtime - start) < (unsigned long)us * (TIMEBASE / 1000000u))
        ;
}

/* console before the serial driver exists: the loader left UART3 running */
#define EARLY_UART_BASE 0x02500C00u

void rt_hw_console_output(const char *str)
{
    for (; *str; str++)
    {
        if (*str == '\n')
        {
            while ((*(volatile rt_uint32_t *)(EARLY_UART_BASE + 0x14u) & 0x20u) == 0u)
                ;
            *(volatile rt_uint32_t *)EARLY_UART_BASE = '\r';
        }
        while ((*(volatile rt_uint32_t *)(EARLY_UART_BASE + 0x14u) & 0x20u) == 0u)
            ;
        *(volatile rt_uint32_t *)EARLY_UART_BASE = *str;
    }
}

/* single core */
rt_weak int rt_hw_cpu_id(void)
{
    return 0;
}

void rt_hw_board_init(void)
{
#ifdef RT_USING_HEAP
#ifdef BSP_PROFILE_USBDISP
    /* the trimmed image is small: the heap starts behind it (and the stacks of the linker script) */
    extern char __stack_end__;

    rt_system_heap_init((void *)RT_ALIGN((rt_ubase_t)&__stack_end__, 4096), (void *)RT_HW_HEAP_END);
#else
    rt_system_heap_init((void *)RT_HW_HEAP_BEGIN, (void *)RT_HW_HEAP_END);
#endif
#endif

    /* device tree: the heap must be up before the nodes are built */
    RT_ASSERT(rt_fdt_prefetch((void *)__dtb_start) == RT_EOK);
    rt_fdt_scan_chosen_stdout();
    rt_fdt_unflatten();

    rt_pic_init();
    rt_pic_irq_init();

    /* the kernel tick, from the machine timer of the CLINT */
    drv_systick_init(TIMEBASE / RT_TICK_PER_SECOND);

#ifdef RT_USING_COMPONENTS_INIT
    rt_components_board_init();
#endif

#if defined(RT_USING_CONSOLE) && defined(RT_USING_DEVICE)
    rt_ofw_console_setup();
#endif
}
