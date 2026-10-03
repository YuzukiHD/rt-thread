/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Glue between the prebuilt decoder archive and RT-Thread.
 *
 * The archive calls a fixed set of C library, clock, reset, interrupt and
 * semaphore functions under the names it was built with. Each group lives in
 * its own glue_*.c file and is implemented on RT-Thread primitives only; the
 * clocks, the reset line and the interrupt come from the device tree node of
 * the video engine, handed over by ve_glue_attach().
 */

#ifndef __VDEC_SUNXI_GLUE_H__
#define __VDEC_SUNXI_GLUE_H__

#include <rtthread.h>
#include <rtdevice.h>
#include "../ve_log.h"

/* Main clock controller number and the clock / reset ids the archive passes in */
#define GLUE_CCU_SYS		2
#define GLUE_CLK_CPU_PLL	1
#define GLUE_CLK_DDR_PLL	3
#define GLUE_CLK_PERI_2X	5
#define GLUE_CLK_PERI_1X	6
#define GLUE_CLK_PERI_480M	8
#define GLUE_CLK_AUDIO1_DIV2	13
#define GLUE_CLK_VE		27
#define GLUE_CLK_BUS_VE		28
#define GLUE_CLK_BUS_VE_M	40
#define GLUE_RST_BUS_VE		6

/* take the clocks, reset line and interrupt of the video engine node */
rt_err_t ve_glue_attach(struct rt_device *dev);

/* resources the glue files share */
struct rt_clk *ve_glue_clk(int id);
struct rt_reset_control *ve_glue_reset(void);
int ve_glue_irq(void);

#endif
