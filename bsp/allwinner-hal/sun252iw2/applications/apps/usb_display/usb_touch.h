/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A multi-touch screen for the PC over USB: a HID interface next to the display interface of the composite device.
 * Windows takes it for a touch screen of its own (no driver to install); the coordinates are absolute in
 * 0..USB_TOUCH_MAX of the screen and mapped by the PC.
 */
#ifndef __USB_TOUCH_H__
#define __USB_TOUCH_H__

#include <rtthread.h>
#include <stdint.h>

#define USB_TOUCH_MAX_CONTACTS  5
#define USB_TOUCH_MAX           32767

#define USB_TOUCH_EP            0x83
#define USB_TOUCH_EP_MPS        64
#define USB_TOUCH_INTERFACE     1

struct usb_touch_point
{
    uint8_t id;         /* number of the finger, kept while it stays down */
    rt_bool_t down;
    uint16_t x, y;      /* 0..USB_TOUCH_MAX */
};

/* the HID interface and endpoint descriptors (to be appended to the configuration descriptor) */
#define USB_TOUCH_DESCRIPTOR_SIZE   (9 + 9 + 7)
uint32_t usb_touch_descriptor(uint8_t *buf, uint8_t interface);

/* add the interface and endpoint to the device (after the display interface) */
void usb_touch_init(uint8_t busid);

/* send one report with up to USB_TOUCH_MAX_CONTACTS fingers; -RT_EBUSY while the last one is on its way */
int usb_touch_send(const struct usb_touch_point *points, int count);

/* start the thread that reads the touch panel and sends its points (usb_ctp.c) */
void usb_ctp_start(void);

#endif
