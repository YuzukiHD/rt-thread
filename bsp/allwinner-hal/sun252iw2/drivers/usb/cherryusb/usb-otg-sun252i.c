/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device mode glue of the CherryUSB MUSB port: the bus gate and reset of the OTG controller, the
 * PHY, the CPU moves the FIFO data (VEND0 PIO) and the interrupt, all described by the device
 * tree node of the controller.
 */

#include <rtthread.h>
#include <rtdevice.h>

#include "usb-glue-sun252i.h"
#include "usb-phy-sun252i.h"
#include "usb_config.h"
#include "usbd_core.h"

#define VEND0       0x43u
/* VEND0 bit 0: the CPU moves the FIFO data (no DMA) */
#define VEND0_PIO   (1u << 0)

static struct
{
    rt_ubase_t base;
    int irq;
    struct rt_clk *clk;
    struct rt_reset_control *rst;
    struct sun252i_usb_phy *phy;
    struct rt_ofw_node *np;
} otg;

rt_ubase_t sun252i_usb_otg_base(void)
{
    return otg.base;
}

static void usbd_isr(int vector, void *param)
{
    USBD_IRQHandler(0);
}

/* strong override of the weak one of port/musb/usb_dc_musb.c */
void usb_dc_low_level_init(void)
{
    /* the PHY node may probe after this one: look it up when the stack starts */
    otg.phy = sun252i_usb_phy_get(otg.np);
    sun252i_usb_phy_acquire(otg.phy, SUN252I_USB_DEVICE);
    rt_clk_prepare_enable(otg.clk);
    rt_reset_control_deassert(otg.rst);

    HWREG8(otg.base + VEND0) = VEND0_PIO;

    rt_pic_attach_irq(otg.irq, usbd_isr, RT_NULL, "usbd", 0);
    rt_pic_irq_unmask(otg.irq);
}

void usb_dc_low_level_deinit(void)
{
    rt_pic_irq_mask(otg.irq);
    rt_pic_detach_irq(otg.irq, RT_NULL);
    if (otg.phy)
        sun252i_usb_phy_release(otg.phy, SUN252I_USB_DEVICE);
}

static rt_err_t otg_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;

    otg.base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    otg.irq = rt_dm_dev_get_irq(dev, 0);
    otg.clk = rt_clk_get_by_index(dev, 0);
    otg.rst = rt_reset_control_get_by_index(dev, 0);
    otg.np = dev->ofw_node;
    if (!otg.base || otg.irq < 0 || rt_is_err_or_null(otg.clk) || rt_is_err_or_null(otg.rst))
    {
        otg.base = 0;
        return -RT_ERROR;
    }

    return RT_EOK;
}

static const struct rt_ofw_node_id otg_ofw_ids[] =
{
    { .compatible = "allwinner,sunxi-musb" },
    { /* sentinel */ }
};

static struct rt_platform_driver otg_driver =
{
    .name = "usb-otg-sun252i",
    .ids = otg_ofw_ids,
    .probe = otg_probe,
};
RT_PLATFORM_DRIVER_EXPORT(otg_driver);
