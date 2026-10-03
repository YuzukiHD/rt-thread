/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __I2S_SUN252I_H__
#define __I2S_SUN252I_H__

#include <rtthread.h>

int sun252i_i2s_open(rt_uint32_t rate, rt_uint32_t period_frames, rt_uint32_t periods, rt_bool_t loopback);
rt_uint8_t *sun252i_i2s_tx_buffer(void);
rt_uint8_t *sun252i_i2s_rx_buffer(void);
rt_uint32_t sun252i_i2s_tx_periods(void);
rt_uint32_t sun252i_i2s_rx_periods(void);
int sun252i_i2s_start(void);
void sun252i_i2s_stop(void);
void sun252i_i2s_close(void);

#endif
