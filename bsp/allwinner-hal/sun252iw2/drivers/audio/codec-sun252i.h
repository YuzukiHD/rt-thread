/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __CODEC_SUN252I_H__
#define __CODEC_SUN252I_H__

#include <rtthread.h>

/* transmit blocks the DMA has finished so far */
rt_uint32_t sun252i_codec_tx_blocks(void);
rt_uint32_t sun252i_codec_rx_blocks(void);
/* samples the DAC has taken */
rt_uint32_t sun252i_codec_dac_count(void);
/* offset of the DMA inside the transmit ring */
rt_size_t sun252i_codec_tx_position(void);

#endif
