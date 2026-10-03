/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C (TWI) controller of the sun252i SoC: the bus is described by its device tree node
 * (registers, clock, reset, pins); transfers poll the controller state.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <rthw.h>

#define TWI_DATA        0x08u
#define TWI_CTL         0x0cu
#define TWI_STATUS      0x10u
#define TWI_CLK         0x14u
#define TWI_SRST        0x18u
#define TWI_EFT         0x1cu

#define CTL_ACK         0x04u
#define CTL_INTFLG      0x08u
#define CTL_STP         0x10u
#define CTL_STA         0x20u
#define CTL_BUSEN       0x40u

#define STATUS_START        0x08u
#define STATUS_RESTART      0x10u
#define STATUS_ADDR_W       0x18u
#define STATUS_ADDR_W_NACK  0x20u
#define STATUS_DATA_W       0x28u
#define STATUS_DATA_W_NACK  0x30u
#define STATUS_ARB_LOST     0x38u
#define STATUS_ADDR_R       0x40u
#define STATUS_ADDR_R_NACK  0x48u
#define STATUS_DATA_R_ACK   0x50u
#define STATUS_DATA_R_NACK  0x58u
#define TWI_TIMEOUT_US      5000u





struct sun_twi
{
    struct rt_i2c_bus_device bus;
    rt_ubase_t base;
    char name[RT_NAME_MAX];
};

static rt_uint32_t twi_read(const struct sun_twi *t, rt_uint32_t off)
{
    return HWREG32(t->base + off);
}

static void twi_write(const struct sun_twi *t, rt_uint32_t off, rt_uint32_t val)
{
    HWREG32(t->base + off) = val;
}

static rt_err_t twi_wait_flag(const struct sun_twi *t, rt_uint32_t expected)
{
    rt_uint32_t n;

    for (n = 0u; n < TWI_TIMEOUT_US; n++)
    {
        if ((twi_read(t, TWI_CTL) & CTL_INTFLG) != 0u)
        {
            rt_uint32_t status = twi_read(t, TWI_STATUS) & 0xffu;

            return status == expected ? RT_EOK : -RT_ERROR;
        }
        rt_hw_us_delay(1u);
    }
    return -RT_ETIMEOUT;
}

static rt_ssize_t twi_master_xfer(struct rt_i2c_bus_device *bus,
        struct rt_i2c_msg msgs[], rt_uint32_t num)
{
    struct sun_twi *t = rt_container_of(bus, struct sun_twi, bus);
    rt_uint32_t i;
    rt_err_t err;

    if (num == 0u)
        return -RT_EINVAL;

    for (i = 0u; i < num; i++)
    {
        struct rt_i2c_msg *msg = &msgs[i];
        rt_bool_t read = (msg->flags & RT_I2C_RD) != 0u;
        rt_uint32_t n;

        if (!(msg->flags & RT_I2C_NO_START))
        {
            rt_uint32_t ctl = twi_read(t, TWI_CTL) | CTL_INTFLG |
                              ((i == 0u) ? CTL_STA : 0u);
            rt_uint32_t addr_state;

            if (i != 0u)
                ctl |= CTL_STA; /* repeated start */
            twi_write(t, TWI_CTL, ctl);
            if (i == 0u)
                err = twi_wait_flag(t, STATUS_START);
            else
                err = twi_wait_flag(t, STATUS_RESTART);
            if (err != RT_EOK)
                goto out;

            twi_write(t, TWI_DATA, ((rt_uint32_t)(msg->addr & 0x7fu) << 1) | (read ? 1u : 0u));
            twi_write(t, TWI_CTL, (twi_read(t, TWI_CTL) & ~CTL_STA) | CTL_INTFLG);
            addr_state = read ? STATUS_ADDR_R : STATUS_ADDR_W;
            err = twi_wait_flag(t, addr_state);
            if (err != RT_EOK)
            {
                /* a NACK leaves INTFLG set: clear it and stop */
                twi_write(t, TWI_CTL, (twi_read(t, TWI_CTL) | CTL_INTFLG | CTL_STP));
                goto out;
            }
        }

        if (read)
        {
            for (n = 0u; n < msg->len; n++)
            {
                rt_uint32_t ctl = twi_read(t, TWI_CTL);

                ctl = (ctl & ~CTL_ACK) | ((n + 1u < msg->len) ? CTL_ACK : 0u);
                ctl |= CTL_INTFLG;
                twi_write(t, TWI_CTL, ctl);
                err = twi_wait_flag(t, (n + 1u < msg->len) ?
                        STATUS_DATA_R_ACK : STATUS_DATA_R_NACK);
                if (err != RT_EOK)
                    goto out;
                msg->buf[n] = (rt_uint8_t)twi_read(t, TWI_DATA);
            }
        }
        else
        {
            for (n = 0u; n < msg->len; n++)
            {
                twi_write(t, TWI_DATA, msg->buf[n]);
                twi_write(t, TWI_CTL, (twi_read(t, TWI_CTL) & ~CTL_STA) | CTL_INTFLG);
                err = twi_wait_flag(t, STATUS_DATA_W);
                if (err != RT_EOK)
                    goto out;
            }
        }
    }
    err = RT_EOK;

out:
    /* stop condition */
    twi_write(t, TWI_CTL, twi_read(t, TWI_CTL) | CTL_INTFLG | CTL_STP);
    for (i = 0u; i < TWI_TIMEOUT_US; i++)
    {
        if ((twi_read(t, TWI_CTL) & CTL_STP) == 0u)
            break;
        rt_hw_us_delay(1u);
    }
    if (err != RT_EOK)
        return err;
    return (rt_ssize_t)num;
}

static const struct rt_i2c_bus_device_ops twi_ops =
{
    twi_master_xfer,
    RT_NULL,
    RT_NULL,
};

static void twi_bus_init(struct sun_twi *t)
{
    /* 100 kHz from the 24 MHz source clock */
    twi_write(t, TWI_EFT, 0u);
    twi_write(t, TWI_SRST, 1u);
    twi_write(t, TWI_CLK, 0x59u);
    twi_write(t, TWI_CTL, CTL_BUSEN);
}

static rt_err_t twi_probe(struct rt_platform_device *pdev)
{
    rt_err_t err;
    struct rt_device *dev = &pdev->parent;
    struct sun_twi *t = rt_calloc(1, sizeof(*t));
    struct rt_clk *clk;
    struct rt_reset_control *rst;

    if (!t)
    {
        return -RT_ENOMEM;
    }

    t->base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    if (!t->base)
    {
        err = -RT_EIO;
        goto _fail;
    }

    clk = rt_clk_get_by_index(dev, 0);
    rst = rt_reset_control_get_by_index(dev, 0);
    if (rt_is_err_or_null(clk) || rt_is_err_or_null(rst))
    {
        err = -RT_ERROR;
        goto _fail;
    }
    rt_reset_control_deassert(rst);
    rt_clk_prepare_enable(clk);
    twi_bus_init(t);

    rt_snprintf(t->name, sizeof(t->name), "i2c%d", pdev->dev_id < 0 ? 0 : pdev->dev_id);
    t->bus.ops = &twi_ops;
    t->bus.parent.ofw_node = dev->ofw_node;
    dev->user_data = t;

    if ((err = rt_i2c_bus_device_register(&t->bus, t->name)))
    {
        goto _fail;
    }

    return RT_EOK;

_fail:
    rt_free(t);

    return err;
}

static const struct rt_ofw_node_id twi_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-i2c" },
    { /* sentinel */ }
};

static struct rt_platform_driver twi_driver =
{
    .name = "i2c-sun252i",
    .ids = twi_ofw_ids,
    .probe = twi_probe,
};
RT_PLATFORM_DRIVER_EXPORT(twi_driver);
