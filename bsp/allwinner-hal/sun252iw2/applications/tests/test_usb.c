/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB: the CherryUSB CDC ACM device (enumerates as a serial port on the host PC) and the EHCI/OHCI
 * host. The single USB port is also the download cable: start the device after the download, the
 * host needs a device plugged into the port instead of the cable.
 */
#include <rtthread.h>
#include <usb/cherryusb/usb-glue-sun252i.h>
#include "test.h"
#include "usbd_core.h"
#include "usbh_core.h"

void cdc_acm_init(uint8_t busid, uintptr_t reg_base);
void cdc_acm_data_send_with_dtr_test(uint8_t busid);

static void usb_device_start(void)
{
    rt_ubase_t base = sun252i_usb_otg_base();
    volatile rt_uint8_t *power = (volatile rt_uint8_t *)(base + 0x40);

    if (!base)
    {
        rt_kprintf("usb: no OTG controller in the device tree\n");
        return;
    }
    cdc_acm_init(0, base);
    /* the host may have given up on the port while the board was running something else:
     * drop the D+ pull-up for a moment so that it sees a fresh attach */
    rt_thread_mdelay(200);
    *power &= ~0x40u;
    rt_thread_mdelay(500);
    *power |= 0x40u;
}
MSH_CMD_EXPORT(usb_device_start, start the USB device (CDC ACM));

/* send a 2048 byte pattern to the host (needs DTR, i.e. an open port) */
static void usb_device_send(void)
{
    cdc_acm_data_send_with_dtr_test(0);
    rt_kprintf("usb: sent 2048 bytes\n");
}
MSH_CMD_EXPORT(usb_device_send, send 2048 bytes to the host over the CDC ACM port);

static void usb_regs(void)
{
    rt_ubase_t base = sun252i_usb_otg_base();

    if (!base)
        return;
    rt_kprintf("usb: POWER %02x DEVCTL %02x INTRUSB %02x INTRUSBE %02x FADDR %02x VEND0 %02x\n",
               HWREG8(base + 0x40), HWREG8(base + 0x60), HWREG8(base + 0x0a), HWREG8(base + 0x0e),
               HWREG8(base + 0x98), HWREG8(base + 0x43));
    rt_kprintf("usb: ISCR %08x PHYCTL %08x\n", HWREG32(base + 0x400), HWREG32(base + 0x410));
    rt_kprintf("usb: dma en %08x sta %08x\n", HWREG32(base + 0x500), HWREG32(base + 0x504));
    for (int ch = 2; ch < 6; ch++)
        rt_kprintf("usb: dma ch%d cfg %08x addr %08x cnt %08x res %08x\n", ch, HWREG32(base + 0x540 + ch * 16),
                   HWREG32(base + 0x544 + ch * 16), HWREG32(base + 0x548 + ch * 16), HWREG32(base + 0x54c + ch * 16));
}
MSH_CMD_EXPORT(usb_regs, dump the MUSB and PHY glue registers);

static void usb_reconnect(void)
{
    volatile rt_uint8_t *power = (volatile rt_uint8_t *)(sun252i_usb_otg_base() + 0x40);

    *power &= ~0x40u;           /* soft disconnect: the host sees the device leave */
    rt_thread_mdelay(500);
    *power |= 0x40u;            /* and attach again */
    rt_kprintf("usb: reconnected, POWER %02x\n", *power);
}
MSH_CMD_EXPORT(usb_reconnect, drop and raise the D+ pull-up);

static void usbh_start(void)
{
    rt_ubase_t base = sun252i_usb_ehci_base();

    if (!base)
    {
        rt_kprintf("usb: no host controller in the device tree\n");
        return;
    }
    usbh_initialize(0, base, RT_NULL);
}
MSH_CMD_EXPORT(usbh_start, start the USB host and enumerate);
