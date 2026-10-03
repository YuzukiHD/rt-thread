/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __OWA_SUN252I_H__
#define __OWA_SUN252I_H__

#include <rtthread.h>

int sun252i_owa_open(rt_uint32_t period_frames, rt_uint32_t periods, rt_bool_t loopback);
rt_uint8_t *sun252i_owa_tx_buffer(void);
rt_uint8_t *sun252i_owa_rx_buffer(void);
rt_uint32_t sun252i_owa_tx_periods(void);
rt_uint32_t sun252i_owa_rx_periods(void);
rt_uint32_t sun252i_owa_reg(rt_uint32_t off);
int sun252i_owa_start(void);
void sun252i_owa_stop(void);
void sun252i_owa_close(void);

#endif
