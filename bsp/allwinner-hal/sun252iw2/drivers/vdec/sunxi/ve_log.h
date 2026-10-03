/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __VDEC_SUNXI_VE_LOG_H__
#define __VDEC_SUNXI_VE_LOG_H__

#define DBG_TAG "vdec"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define LOG_ERR LOG_E
#define LOG_WRN LOG_W
#define LOG_INF LOG_I
#define LOG_DBG LOG_D

#define VE_LOG_DECLARE()	_Static_assert(1, "log")

#endif /* __VDEC_SUNXI_VE_LOG_H__ */
