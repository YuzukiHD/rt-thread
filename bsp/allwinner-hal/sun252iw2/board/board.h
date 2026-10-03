/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __BOARD_H__
#define __BOARD_H__

#include <rtthread.h>

/* whole cache maintenance (board/cache.c), beside the range operations of rthw.h */
void rt_hw_cpu_dcache_clean_all(void);
void rt_hw_cpu_dcache_invalidate_all(void);
void rt_hw_cpu_dcache_clean_invalidate_all(void);
void rt_hw_cpu_icache_invalidate_all(void);

#endif
