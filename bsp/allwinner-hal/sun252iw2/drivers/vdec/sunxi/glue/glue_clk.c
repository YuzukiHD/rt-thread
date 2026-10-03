/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Clock handles for the archive. It asks for the module clock, the two bus
 * gates and a few PLL outputs by number; each one maps to a clock of the
 * device tree. Only the module clock has a settable rate: the clock driver
 * picks its source and dividers, so a parent request is accepted and ignored.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "glue.h"

struct glue_clk {
	uint16_t id;
};

static struct glue_clk clks[] = {
	{ GLUE_CLK_CPU_PLL },    { GLUE_CLK_DDR_PLL },       { GLUE_CLK_PERI_2X },
	{ GLUE_CLK_PERI_1X },    { GLUE_CLK_PERI_480M },     { GLUE_CLK_AUDIO1_DIV2 },
	{ GLUE_CLK_VE },         { GLUE_CLK_BUS_VE },        { GLUE_CLK_BUS_VE_M },
};

int hal_n_clk_get(uint8_t cc_id, uint16_t clk_id, void **clk)
{
	if (cc_id != GLUE_CCU_SYS) {
		return -EINVAL;
	}
	for (size_t i = 0; i < RT_ARRAY_SIZE(clks); i++) {
		if (clks[i].id == clk_id) {
			*clk = &clks[i];
			return 0;
		}
	}
	LOG_WRN("unknown clock %u", clk_id);

	return -ENOENT;
}

int hal_n_clk_enable(void *clk)
{
	struct rt_clk *c = ve_glue_clk(((struct glue_clk *)clk)->id);

	return c ? -rt_clk_prepare_enable(c) : 0;
}

int hal_n_clk_disable(void *clk)
{
	struct rt_clk *c = ve_glue_clk(((struct glue_clk *)clk)->id);

	if (c) {
		rt_clk_disable_unprepare(c);
	}

	return 0;
}

int hal_n_clk_set_parent(void *clk, void *parent)
{
	return ((struct glue_clk *)clk)->id == GLUE_CLK_VE ? 0 : -ENOTSUP;
}

int hal_n_clk_set_freq(void *clk, uint32_t freq)
{
	struct glue_clk *c = clk;

	if (c->id != GLUE_CLK_VE || freq == 0U) {
		return -EINVAL;
	}

	return -rt_clk_set_rate(ve_glue_clk(c->id), freq);
}

int hal_n_clk_get_freq(void *clk, uint32_t *freq)
{
	struct rt_clk *c = ve_glue_clk(((struct glue_clk *)clk)->id);

	*freq = c ? (uint32_t)rt_clk_get_rate(c) : 0U;

	return 0;
}

int hal_n_clk_round_freq(void *clk, uint32_t *freq)
{
	struct glue_clk *c = clk;
	rt_base_t r;

	if (c->id != GLUE_CLK_VE || *freq == 0U) {
		return -EINVAL;
	}
	r = rt_clk_round_rate(ve_glue_clk(c->id), *freq);
	if (r <= 0) {
		return -EINVAL;
	}
	*freq = (uint32_t)r;

	return 0;
}
