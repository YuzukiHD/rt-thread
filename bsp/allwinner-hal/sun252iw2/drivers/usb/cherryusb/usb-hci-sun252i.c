/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Low level glue of the CherryUSB EHCI/OHCI host: bus gates and resets, PHY, VBUS, the two
 * interrupts and the data cache maintenance the controllers need, from the device tree node of
 * the host controller pair.
 */

#include <rtthread.h>
#include <rtdevice.h>

#include "usb-glue-sun252i.h"
#include "usb-phy-sun252i.h"
#include "usbh_core.h"
#include "usb_hc_ehci.h"
#ifdef CONFIG_USB_EHCI_WITH_OHCI
#include "usb_hc_ohci.h"
#endif

static struct
{
    rt_ubase_t ehci;
    int ehci_irq;
    int ohci_irq;
    struct rt_clk *clk[2];
    struct rt_reset_control *rst[2];
    struct sun252i_usb_phy *phy;
    struct rt_ofw_node *np;
} hci;

rt_ubase_t sun252i_usb_ehci_base(void)
{
    return hci.ehci;
}

/* cache maintenance the CherryUSB host core calls around DMA */
void usb_dcache_clean(uintptr_t addr, size_t size)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)addr, size);
}

void usb_dcache_invalidate(uintptr_t addr, size_t size)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, (void *)addr, size);
}

void usb_dcache_flush(uintptr_t addr, size_t size)
{
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)addr, size);
}

static void ehci_isr(int vector, void *param)
{
    USBH_IRQHandler(0);
}

#ifdef CONFIG_USB_EHCI_WITH_OHCI
static void ohci_isr(int vector, void *param)
{
    OHCI_IRQHandler(0);
}
#endif

/* strong override of the weak one of port/ehci/usb_hc_ehci.c */
void usb_hc_low_level_init(struct usbh_bus *bus)
{
    int i;

    /* the PHY node may probe after this one: look it up when the stack starts */
    hci.phy = sun252i_usb_phy_get(hci.np);
    sun252i_usb_phy_acquire(hci.phy, SUN252I_USB_HOST);
    for (i = 0; i < 2; i++)
    {
        rt_clk_prepare_enable(hci.clk[i]);
        rt_reset_control_deassert(hci.rst[i]);
    }

    rt_pic_attach_irq(hci.ehci_irq, ehci_isr, bus, "ehci", 0);
    rt_pic_irq_unmask(hci.ehci_irq);
#ifdef CONFIG_USB_EHCI_WITH_OHCI
    rt_pic_attach_irq(hci.ohci_irq, ohci_isr, bus, "ohci", 0);
    rt_pic_irq_unmask(hci.ohci_irq);
#endif

    USB_LOG_INFO("usb host ready\n");
}

/* strong override of the weak one of port/ehci/usb_hc_ehci.c */
uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port)
{
    uint32_t regval = EHCI_HCOR->portsc[port - 1];

    if ((regval & EHCI_PORTSC_LSTATUS_MASK) == EHCI_PORTSC_LSTATUS_KSTATE)
        return USB_SPEED_LOW;
    if (regval & EHCI_PORTSC_PE)
        return USB_SPEED_HIGH;
    return USB_SPEED_FULL;
}

static rt_err_t hci_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    int i;

    hci.ehci = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    hci.ehci_irq = rt_dm_dev_get_irq(dev, 0);
    hci.ohci_irq = rt_dm_dev_get_irq(dev, 1);
    hci.np = dev->ofw_node;
    for (i = 0; i < 2; i++)
    {
        hci.clk[i] = rt_clk_get_by_index(dev, i);
        hci.rst[i] = rt_reset_control_get_by_index(dev, i);
        if (rt_is_err_or_null(hci.clk[i]) || rt_is_err_or_null(hci.rst[i]))
            return -RT_ERROR;
    }
    if (!hci.ehci || hci.ehci_irq < 0 || hci.ohci_irq < 0)
    {
        hci.ehci = 0;
        return -RT_ERROR;
    }

    return RT_EOK;
}

static const struct rt_ofw_node_id hci_ofw_ids[] =
{
    { .compatible = "allwinner,sunxi-ehci" },
    { /* sentinel */ }
};

static struct rt_platform_driver hci_driver =
{
    .name = "usb-hci-sun252i",
    .ids = hci_ofw_ids,
    .probe = hci_probe,
};
RT_PLATFORM_DRIVER_EXPORT(hci_driver);
