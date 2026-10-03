/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Device tree resources of the video engine for the other glue files */

#include <errno.h>
#include "glue.h"

static struct rt_clk *ve_clks[6];
static struct rt_reset_control *ve_rst;
static int ve_irq = -1;

rt_err_t ve_glue_attach(struct rt_device *dev)
{
	static const char *const names[] = { "mod", "bus", "mbus", "peri2x", "peri1x", "peri480m" };
	unsigned int i;

	for (i = 0; i < RT_ARRAY_SIZE(names); i++) {
		ve_clks[i] = rt_clk_get_by_name(dev, names[i]);
		if (rt_is_err(ve_clks[i]))
			ve_clks[i] = RT_NULL;
	}
	ve_rst = rt_reset_control_get_by_index(dev, 0);
	if (rt_is_err(ve_rst))
		ve_rst = RT_NULL;
	ve_irq = rt_dm_dev_get_irq(dev, 0);

	return (ve_clks[0] && ve_clks[1] && ve_clks[2] && ve_rst && ve_irq >= 0) ? RT_EOK : -RT_ERROR;
}

/* the clock behind an id of the archive, NULL for the PLLs this SoC glue does not model */
struct rt_clk *ve_glue_clk(int id)
{
	switch (id) {
	case GLUE_CLK_VE:		return ve_clks[0];
	case GLUE_CLK_BUS_VE:		return ve_clks[1];
	case GLUE_CLK_BUS_VE_M:		return ve_clks[2];
	case GLUE_CLK_PERI_2X:		return ve_clks[3];
	case GLUE_CLK_PERI_1X:		return ve_clks[4];
	case GLUE_CLK_PERI_480M:	return ve_clks[5];
	default:			return RT_NULL;
	}
}

struct rt_reset_control *ve_glue_reset(void)
{
	return ve_rst;
}

int ve_glue_irq(void)
{
	return ve_irq;
}
