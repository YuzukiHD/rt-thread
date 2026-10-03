/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The one reset line of the video engine, handed out as an opaque handle */

#include <errno.h>
#include <stdint.h>
#include "glue.h"

static int ve_reset;

int hal_rst_get(uint8_t rc_id, uint16_t rst_id, void **rst)
{
	if (rc_id != GLUE_CCU_SYS || rst_id != GLUE_RST_BUS_VE)
		return -ENOENT;
	*rst = &ve_reset;

	return 0;
}

int hal_rst_put(void *rst)
{
	(void)rst;
	return 0;
}

int hal_rst_assert(void *rst)
{
	(void)rst;
	return -rt_reset_control_assert(ve_glue_reset());
}

int hal_rst_deassert(void *rst)
{
	(void)rst;
	return -rt_reset_control_deassert(ve_glue_reset());
}
