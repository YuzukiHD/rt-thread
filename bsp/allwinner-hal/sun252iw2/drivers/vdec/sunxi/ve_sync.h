/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __VDEC_SUNXI_VE_SYNC_H__
#define __VDEC_SUNXI_VE_SYNC_H__

#include <rtthread.h>

typedef rt_mutex_t ve_mutex_t;

static inline void ve_mutex_init(ve_mutex_t *m)
{
	*m = rt_mutex_create("ve", RT_IPC_FLAG_PRIO);
}

static inline void ve_mutex_lock(ve_mutex_t *m)
{
	rt_mutex_take(*m, RT_WAITING_FOREVER);
}

static inline void ve_mutex_unlock(ve_mutex_t *m)
{
	rt_mutex_release(*m);
}

static inline void ve_mutex_destroy(ve_mutex_t *m)
{
	if (*m) {
		rt_mutex_delete(*m);
		*m = RT_NULL;
	}
}

#endif /* __VDEC_SUNXI_VE_SYNC_H__ */
