/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Routes the JTAG port of the core to its pins. The platform core applies the pin group of the
 * node ("default" state) before the probe; nothing else is left to do.
 */
#include <rtthread.h>
#include <rtdevice.h>

#define PIO_PF_CFG0 (0x02000000u + 5u * 0x30u)

static rt_err_t jtag_probe(struct rt_platform_device *pdev)
{
    rt_kprintf("jtag: PF0 TMS, PF1 TDI, PF3 TDO, PF5 TCK routed (PF_CFG0 %08x)\n",
               *(volatile rt_uint32_t *)PIO_PF_CFG0);

    return RT_EOK;
}

static const struct rt_ofw_node_id jtag_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-jtag" },
    { /* sentinel */ }
};

static struct rt_platform_driver jtag_driver =
{
    .name = "jtag-sun252i",
    .ids = jtag_ofw_ids,
    .probe = jtag_probe,
};
RT_PLATFORM_DRIVER_EXPORT(jtag_driver);
