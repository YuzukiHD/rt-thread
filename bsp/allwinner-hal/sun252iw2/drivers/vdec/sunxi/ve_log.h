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

/* errors and warnings are always printed, the rest follows the debug configuration */
#define LOG_ERR(fmt, ...) rt_kprintf("[vdec E] " fmt "\n", ##__VA_ARGS__)
#define LOG_WRN(fmt, ...) rt_kprintf("[vdec W] " fmt "\n", ##__VA_ARGS__)
#define LOG_INF LOG_I
#define LOG_DBG LOG_D

#define VE_LOG_DECLARE()	_Static_assert(1, "log")

#endif /* __VDEC_SUNXI_VE_LOG_H__ */
