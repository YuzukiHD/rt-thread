/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __MIPI_DBI_SUN252I_H__
#define __MIPI_DBI_SUN252I_H__

#include <rtthread.h>

#ifndef BIT
#define BIT(n)  (1u << (n))
#endif

rt_bool_t sun252i_dbi_ready(void);
int sun252i_dbi_reset(rt_uint32_t delay_ms);
int sun252i_dbi_command_write(rt_uint8_t cmd, const rt_uint8_t *data, rt_size_t len);
int sun252i_dbi_write_display(const rt_uint8_t *framebuf, rt_size_t size);

#endif
