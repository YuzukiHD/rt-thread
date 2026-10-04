/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __SID_SUN252I_H__
#define __SID_SUN252I_H__

#include <rtthread.h>

#define SUN252I_SID_BITS    512
#define SUN252I_SID_CHIPID_BYTES 16

/* read len bytes of fuses from byte offset; both must be multiples of 4 (returns len or a negative error) */
rt_ssize_t sun252i_sid_read(rt_uint32_t offset, void *buf, rt_size_t len);
/* the 128 bit chip identifier (fuse words 0..3), at most 16 bytes (returns the count or a negative error) */
rt_ssize_t sun252i_sid_chipid(rt_uint8_t *buf, rt_size_t len);

#endif
