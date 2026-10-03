/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef __USB_GLUE_SUN252I_H__
#define __USB_GLUE_SUN252I_H__

#include <rtthread.h>

/* register window of the controllers once their platform driver probed, 0 before */
rt_ubase_t sun252i_usb_otg_base(void);
rt_ubase_t sun252i_usb_ehci_base(void);

#endif
