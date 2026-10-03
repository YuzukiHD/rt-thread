/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __DMA_SUN252I_H__
#define __DMA_SUN252I_H__

#include <drivers/dma.h>

/* bytes the channel moved of the buffer of the running transfer (the position inside a ring) */
rt_size_t sun252i_dma_position(struct rt_dma_chan *chan);

/* bytes still to move of the current block */
rt_size_t sun252i_dma_pending(struct rt_dma_chan *chan);

#endif
