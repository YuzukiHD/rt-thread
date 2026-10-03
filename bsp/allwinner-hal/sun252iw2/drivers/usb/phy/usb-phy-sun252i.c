/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UTMI PHY of the sun252i SoC and the glue registers around it.
 *
 * One PHY feeds the OTG (device) controller and the EHCI/OHCI host pair; a role is taken
 * exclusively. The controllers also need the FIFO RAM of the OTG block: the SRAM owner bit in the
 * system controller clears the remap. VBUS of the host role is a GPIO of the node.
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "usb-phy-sun252i.h"

void *rt_ioremap(void *paddr, size_t size);

#define BIT(n) (1u << (n))

#define OTG_ISCR            0x00
#define OTG_PHYCTRL         0x10
#define OTG_PHYSEL          0x20

#define ISCR_DPDM_PULLUP_EN BIT(16)
#define ISCR_ID_PULLUP_EN   BIT(17)
#define ISCR_FORCE_ID_HIGH  (0x3u << 14)
#define ISCR_FORCE_VBUS_HIGH (0x3u << 12)
#define ISCR_CHANGE_FLAGS   (BIT(6) | BIT(5) | BIT(4))

#define PHYCTRL_SIDDQ       BIT(3)
#define PHYCTRL_VBUSVLDEXT  BIT(5)
#define PHYSEL_OTG          BIT(0)

/* host controller PHY/SIE window */
#define HCI_PASSBY          0x00
#define HCI_PHYCTRL         0x10
#define HCI_PASSBY_BITS     (BIT(10) | BIT(9) | BIT(8) | BIT(0))  /* INCR8/4/x align, ULPI bypass */

struct sun252i_usb_phy
{
    rt_ubase_t otg;
    rt_ubase_t hci;
    rt_ubase_t sram_remap;
    rt_uint32_t sram_remap_clear;
    rt_base_t vbus;
    rt_bool_t in_use;
    enum sun252i_usb_role role;
};

static rt_uint32_t rd(rt_ubase_t a) { return HWREG32(a); }
static void wr(rt_ubase_t a, rt_uint32_t v) { HWREG32(a) = v; }

static void vbus_set(struct sun252i_usb_phy *p, rt_bool_t on)
{
    if (p->vbus < 0)
        return;
    rt_pin_mode(p->vbus, PIN_MODE_OUTPUT);
    rt_pin_write(p->vbus, on ? PIN_HIGH : PIN_LOW);
}

struct sun252i_usb_phy *sun252i_usb_phy_get(struct rt_ofw_node *controller)
{
    struct rt_ofw_node *np = rt_ofw_parse_phandle(controller, "phys", 0);

    if (!np)
        return RT_NULL;
    if (!rt_ofw_data(np))
        rt_platform_ofw_request(np);

    return rt_ofw_data(np);
}

int sun252i_usb_phy_acquire(struct sun252i_usb_phy *p, enum sun252i_usb_role role)
{
    if (p->in_use)
        return p->role == role ? 0 : -RT_EBUSY;

    /* the controllers need the FIFO RAM of the OTG block before they run */
    if (p->sram_remap)
        wr(p->sram_remap, rd(p->sram_remap) & ~p->sram_remap_clear);

    if (role == SUN252I_USB_DEVICE)
    {
        /* no ID or VBUS comparator is wired: report the B device, VBUS valid */
        wr(p->otg + OTG_ISCR, (rd(p->otg + OTG_ISCR) & ~ISCR_CHANGE_FLAGS) | ISCR_DPDM_PULLUP_EN |
           ISCR_ID_PULLUP_EN | ISCR_FORCE_ID_HIGH | ISCR_FORCE_VBUS_HIGH);
        wr(p->otg + OTG_PHYCTRL, (rd(p->otg + OTG_PHYCTRL) | PHYCTRL_VBUSVLDEXT) & ~PHYCTRL_SIDDQ);
        wr(p->otg + OTG_PHYSEL, rd(p->otg + OTG_PHYSEL) | PHYSEL_OTG);
    }
    else
    {
        /* route the PHY to the host controllers and power it */
        wr(p->otg + OTG_PHYSEL, rd(p->otg + OTG_PHYSEL) & ~PHYSEL_OTG);
        wr(p->hci + HCI_PHYCTRL, rd(p->hci + HCI_PHYCTRL) & ~PHYCTRL_SIDDQ);
        wr(p->hci + HCI_PASSBY, rd(p->hci + HCI_PASSBY) | HCI_PASSBY_BITS);
        vbus_set(p, RT_TRUE);
    }

    p->in_use = RT_TRUE;
    p->role = role;

    return 0;
}

void sun252i_usb_phy_release(struct sun252i_usb_phy *p, enum sun252i_usb_role role)
{
    if (!p->in_use || p->role != role)
        return;

    if (role == SUN252I_USB_HOST)
        vbus_set(p, RT_FALSE);
    /* power the PHY down */
    wr(p->otg + OTG_PHYCTRL, rd(p->otg + OTG_PHYCTRL) | PHYCTRL_SIDDQ);
    wr(p->hci + HCI_PHYCTRL, rd(p->hci + HCI_PHYCTRL) | PHYCTRL_SIDDQ);
    p->in_use = RT_FALSE;
}

static rt_err_t phy_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    struct sun252i_usb_phy *p = rt_calloc(1, sizeof(*p));
    struct rt_clk *clk;
    struct rt_reset_control *rst;
    rt_uint32_t v;

    if (!p)
        return -RT_ENOMEM;

    p->otg = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    p->hci = (rt_ubase_t)rt_dm_dev_iomap(dev, 1);
    clk = rt_clk_get_by_index(dev, 0);
    rst = rt_reset_control_get_by_index(dev, 0);
    if (!p->otg || !p->hci || rt_is_err_or_null(clk) || rt_is_err_or_null(rst))
    {
        rt_free(p);
        return -RT_ERROR;
    }

    /* gate and reset of the PHY block; a set reset bit releases the block */
    rt_clk_prepare_enable(clk);
    rt_reset_control_deassert(rst);

    if (!rt_dm_dev_prop_read_u32(dev, "sram-remap-reg", &v))
        p->sram_remap = (rt_ubase_t)rt_ioremap((void *)(rt_ubase_t)v, 4);
    rt_dm_dev_prop_read_u32(dev, "sram-remap-clear", &p->sram_remap_clear);
    p->vbus = rt_pin_get_named_pin(dev, "vbus", 0, RT_NULL, RT_NULL);
    p->in_use = RT_FALSE;

    dev->user_data = p;
    rt_dm_dev_bind_fwdata(dev, RT_NULL, p);

    return RT_EOK;
}

static const struct rt_ofw_node_id phy_ofw_ids[] =
{
    { .compatible = "allwinner,sunxi-usb-phy" },
    { /* sentinel */ }
};

static struct rt_platform_driver phy_driver =
{
    .name = "usb-phy-sun252i",
    .ids = phy_ofw_ids,
    .probe = phy_probe,
};
RT_PLATFORM_DRIVER_EXPORT(phy_driver);
