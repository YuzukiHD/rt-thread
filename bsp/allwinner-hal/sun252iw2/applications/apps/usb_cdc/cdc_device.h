/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The CDC ACM device of the board: hooks for whoever wants to watch its data (the benchmark), and a source that
 * streams a pattern to the host.
 */
#ifndef __CDC_DEVICE_H__
#define __CDC_DEVICE_H__

#include <rtthread.h>
#include <stdint.h>

struct usb_cdc_hooks
{
    /* data arrived on the OUT endpoint (the buffer is reused after the call) */
    void (*rx)(const uint8_t *data, uint32_t len);
    /* an IN transfer of @len bytes is complete */
    void (*tx)(uint32_t len);
};

void usb_cdc_set_hooks(const struct usb_cdc_hooks *hooks);
void usb_cdc_source(rt_bool_t on, rt_bool_t pattern);
void cdc_acm_init(uint8_t busid, uintptr_t reg_base);
void cdc_acm_data_send_with_dtr_test(uint8_t busid);

#endif
