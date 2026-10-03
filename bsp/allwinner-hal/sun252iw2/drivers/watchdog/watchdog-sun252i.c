/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Watchdog of the sun252i SoC: the timeout is one of eleven hardware periods, the
 * chip is reset when it expires.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/dev_watchdog.h>

#define WDT_IRQ_EN  0x00u
#define WDT_STATUS  0x04u
#define WDT_CTL     0x10u
#define WDT_CFG     0x14u
#define WDT_MODE    0x18u
#define WDT_OUT_CFG 0x1cu

#define WDT_MODE_ENABLE 0x01u
#define WDT_TIMEOUT_SHIFT 4u
#define WDT_TIMEOUT_MASK 0x000000f0u
#define WDT_CFG_RESET 0x01u
#define WDT_KEY 0x16aa0000u
#define WDT_CTL_RESTART 0x01u
#define WDT_CTL_KEY 0x14aeu /* 0x0a57 << 1 */
#define WDT_OUT_RESET_PULSE 0x3fu



static const rt_uint32_t wdt_periods_ms[] =
{
    1000u, 2000u, 3000u, 4000u, 5000u, 6000u, 8000u, 10000u, 12000u, 14000u,
    16000u,
};


struct sun_wdt
{
    rt_watchdog_t parent;
    rt_ubase_t base;
    rt_uint8_t code;
    rt_uint32_t ms;
    rt_bool_t installed, started;
};



static rt_uint32_t wdt_read(struct sun_wdt *w, rt_uint32_t off)
{
    return HWREG32(w->base + off);
}

static void wdt_write(struct sun_wdt *w, rt_uint32_t off, rt_uint32_t val)
{
    HWREG32(w->base + off) = val;
}

static void wdt_disable_hw(struct sun_wdt *w)
{
    rt_uint32_t mode = wdt_read(w, WDT_MODE);

    mode &= ~WDT_MODE_ENABLE;
    wdt_write(w, WDT_MODE, mode | WDT_KEY);
    wdt_write(w, WDT_IRQ_EN, 0u);
    wdt_write(w, WDT_STATUS, 1u);
    wdt_write(w, WDT_CFG, WDT_KEY);
}

static rt_err_t wdt_init(rt_watchdog_t *wdt)
{
    wdt_disable_hw((struct sun_wdt *)wdt);
    return RT_EOK;
}

static rt_err_t wdt_control(rt_watchdog_t *wdt, int cmd, void *arg)
{
    struct sun_wdt *w = (struct sun_wdt *)wdt;
    rt_err_t ret = RT_EOK;
    switch (cmd)
    {
    case RT_DEVICE_CTRL_WDT_SET_TIMEOUT:
    {
        rt_uint32_t ms = *(rt_uint32_t *)arg * 1000u;
        rt_size_t i;

        if (ms == 0u || w->started)
            return -RT_EINVAL;
        if (w->installed)
            return -RT_EBUSY;

        w->code = 0u;
        w->ms = 0u;
        /* the first hardware period not shorter than the requested one */
        for (i = 0; i < sizeof(wdt_periods_ms) / sizeof(wdt_periods_ms[0]); i++)
        {
            if (ms <= wdt_periods_ms[i])
            {
                w->code = (rt_uint8_t)(i + 1u);
                w->ms = wdt_periods_ms[i];
                break;
            }
        }
        if (w->ms == 0u)
            ret = -RT_EINVAL;
        else
            w->installed = RT_TRUE;
        break;
    }
    case RT_DEVICE_CTRL_WDT_START:
    {
        rt_uint32_t mode;

        if (!w->installed)
            return -RT_EINVAL;
        if (w->started)
            return -RT_EBUSY;

        wdt_disable_hw(w);
        wdt_write(w, WDT_OUT_CFG, WDT_OUT_RESET_PULSE);
        wdt_write(w, WDT_CFG, WDT_KEY | WDT_CFG_RESET);
        mode = WDT_KEY | ((rt_uint32_t)w->code << WDT_TIMEOUT_SHIFT) |
               WDT_MODE_ENABLE;
        wdt_write(w, WDT_MODE, mode);
        wdt_write(w, WDT_CTL, WDT_CTL_KEY | WDT_CTL_RESTART);
        w->started = RT_TRUE;
        break;
    }
    case RT_DEVICE_CTRL_WDT_STOP:
        if ((wdt_read(w, WDT_MODE) & WDT_MODE_ENABLE) == 0u)
            return -RT_ERROR;
        wdt_disable_hw(w);
        w->started = RT_FALSE;
        w->installed = RT_FALSE;
        break;
    case RT_DEVICE_CTRL_WDT_KEEPALIVE:
        if (!w->started || (wdt_read(w, WDT_MODE) & WDT_MODE_ENABLE) == 0u)
            return -RT_EINVAL;
        wdt_write(w, WDT_CTL, WDT_CTL_KEY | WDT_CTL_RESTART);
        break;
    case RT_DEVICE_CTRL_WDT_GET_TIMEOUT:
        *(rt_uint32_t *)arg = w->ms / 1000u;
        break;
    default:
        ret = -RT_EINVAL;
        break;
    }
    return ret;
}

static const struct rt_watchdog_ops wdt_ops =
{
    wdt_init,
    wdt_control,
};

static rt_err_t wdt_probe(struct rt_platform_device *pdev)
{
    struct rt_device *dev = &pdev->parent;
    struct sun_wdt *w = rt_calloc(1, sizeof(*w));
    rt_err_t err;

    if (!w)
    {
        return -RT_ENOMEM;
    }

    w->base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    if (!w->base)
    {
        rt_free(w);

        return -RT_EIO;
    }

    w->parent.ops = &wdt_ops;
    w->parent.parent.ofw_node = dev->ofw_node;
    dev->user_data = w;

    rt_dm_dev_set_name_auto(&w->parent.parent, "wdt");
    err = rt_hw_watchdog_register(&w->parent, rt_dm_dev_get_name(&w->parent.parent), RT_DEVICE_FLAG_DEACTIVATE, w);
    if (err)
    {
        rt_free(w);
    }

    return err;
}

static const struct rt_ofw_node_id wdt_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-wdt" },
    { /* sentinel */ }
};

static struct rt_platform_driver wdt_driver =
{
    .name = "watchdog-sun252i",
    .ids = wdt_ofw_ids,
    .probe = wdt_probe,
};
RT_PLATFORM_DRIVER_EXPORT(wdt_driver);
