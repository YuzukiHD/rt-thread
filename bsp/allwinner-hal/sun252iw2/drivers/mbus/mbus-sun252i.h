/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __MBUS_SUN252I_H__
#define __MBUS_SUN252I_H__

#include <rtthread.h>

#define MBUS_PMU_DMA    3
#define MBUS_PMU_TOTAL  15
#define MBUS_PMU_COUNT  16
#define MBUS_MASTER_COUNT 16

const char *mbus_pmu_name(int counter);
rt_uint32_t mbus_traffic(int counter);
int mbus_set_priority(rt_uint32_t master, rt_uint32_t prio);
int mbus_get_priority(rt_uint32_t master, rt_uint32_t *prio);
int mbus_set_limit(rt_uint32_t master, rt_uint32_t mbps);
int mbus_get_limit(rt_uint32_t master, rt_uint32_t *mbps);

#endif
