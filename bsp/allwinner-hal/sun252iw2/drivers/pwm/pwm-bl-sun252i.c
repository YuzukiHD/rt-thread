/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Panel backlight PWM block of the sun252i SoC: two ports, port 0 drives the digital PWM
 * with an analog compare, port 1 is a pure analog current sink. The registered PWM device
 * has one channel; its duty cycle maps to the load current 0..200.
 */
#include <rtthread.h>
#include <rtdevice.h>

#define PWMBL_PORT_STRIDE       0x200u
#define PWMBL_CTRL              0x000
#define   PWMBL_CTRL_ENABLE     (1u << 0)
#define   PWMBL_CTRL_CLK_GATING (1u << 1)
#define   PWMBL_CTRL_MODE_DIG_ANA (1u << 2)
#define PWMBL_CLK_CFG           0x004
#define PWMBL_PRD               0x008
#define PWMBL_ACT_CYCLE         0x00c
#define PWMBL_ACT_STEP          0x010
#define PWMBL_ACT_UP_CYCLE      0x014
#define PWMBL_OCP_FLT_V         0x018
#define PWMBL_INT_EN            0x040
#define PWMBL_ANA0              0x050
#define PWMBL_ANA1              0x054
#define PWMBL_ANA2              0x058
#define   ANA1_MAX_BRIGHT_MASK  (3u << 0)
#define   ANA1_LOAD_CUR_MASK    (0xffu << 16)
#define   ANA1_OCP_EN           (1u << 31)
#define   OCP_FLT_V_MASK        0xffu
#define   OCP_FLT_EN            (1u << 31)

#define PWMBL_MAX_LOAD_CUR      200u
#define PWMBL_MOD_RATE          400000000u
#define PWMBL_INT_MASK          ((1u << 0) | (1u << 2) | (1u << 3))

struct sun_pwm_bl
{
    struct rt_device_pwm pwm;
    rt_ubase_t base;
    rt_uint32_t level;
    rt_bool_t enabled;
};

static rt_uint32_t bl_read(struct sun_pwm_bl *b, rt_uint32_t port, rt_uint32_t off)
{
    return HWREG32(b->base + port * PWMBL_PORT_STRIDE + off);
}

static void bl_write(struct sun_pwm_bl *b, rt_uint32_t port, rt_uint32_t off, rt_uint32_t v)
{
    HWREG32(b->base + port * PWMBL_PORT_STRIDE + off) = v;
}

static void bl_update(struct sun_pwm_bl *b, rt_uint32_t port, rt_uint32_t off,
        rt_uint32_t mask, rt_uint32_t v)
{
    bl_write(b, port, off, (bl_read(b, port, off) & ~mask) | (v & mask));
}

static void bl_level(struct sun_pwm_bl *b, rt_uint32_t level)
{
    if (level > PWMBL_MAX_LOAD_CUR)
        level = PWMBL_MAX_LOAD_CUR;
    b->level = level;
    bl_update(b, 0u, PWMBL_CTRL, PWMBL_CTRL_ENABLE, level ? PWMBL_CTRL_ENABLE : 0u);
    bl_update(b, 1u, PWMBL_CTRL, PWMBL_CTRL_ENABLE, level ? PWMBL_CTRL_ENABLE : 0u);
    bl_update(b, 0u, PWMBL_ANA1, ANA1_LOAD_CUR_MASK, level << 16);
}

static void bl_hw_init(struct sun_pwm_bl *b)
{
    /* port 1: pure analog, 750 kHz */
    bl_update(b, 1u, PWMBL_CTRL, PWMBL_CTRL_MODE_DIG_ANA, 0u);
    bl_update(b, 1u, PWMBL_ANA2, 0x3u, 1u);
    bl_update(b, 1u, PWMBL_INT_EN, PWMBL_INT_MASK, PWMBL_INT_MASK);
    bl_update(b, 1u, PWMBL_ANA1, ANA1_OCP_EN, 0u);
    bl_update(b, 1u, PWMBL_OCP_FLT_V, OCP_FLT_V_MASK, 0xffu);
    bl_update(b, 1u, PWMBL_OCP_FLT_V, OCP_FLT_EN, OCP_FLT_EN);
    bl_update(b, 1u, PWMBL_ANA1, ANA1_MAX_BRIGHT_MASK, 2u);
    bl_update(b, 1u, PWMBL_ANA1, ANA1_LOAD_CUR_MASK, 0u);
    bl_update(b, 1u, PWMBL_ANA0, 1u, 1u);
    bl_update(b, 1u, PWMBL_ANA1, ANA1_LOAD_CUR_MASK, 0xffu << 16);

    /* port 0: digital + analog compare, clocked from the peripheral PLL */
    bl_update(b, 0u, PWMBL_CTRL, PWMBL_CTRL_MODE_DIG_ANA, PWMBL_CTRL_MODE_DIG_ANA);
    bl_update(b, 0u, PWMBL_CLK_CFG, (3u << 30), 2u << 30);
    bl_update(b, 0u, PWMBL_CLK_CFG, (0x7fu << 8), 0u);
    bl_update(b, 0u, PWMBL_CLK_CFG, 0xfu, 0u);
    bl_write(b, 0u, PWMBL_PRD, 0x215u);
    bl_update(b, 0u, PWMBL_ACT_CYCLE, 0xffffu, 0x140u);
    bl_update(b, 0u, PWMBL_ACT_CYCLE, 0xffffu << 16, 0x1au << 16);
    bl_write(b, 0u, PWMBL_ACT_STEP, 1u);
    bl_write(b, 0u, PWMBL_ACT_UP_CYCLE, 1u);
    bl_update(b, 0u, PWMBL_ANA1, ANA1_OCP_EN, 0u);
    bl_update(b, 0u, PWMBL_OCP_FLT_V, OCP_FLT_V_MASK, 0xffu);
    bl_update(b, 0u, PWMBL_OCP_FLT_V, OCP_FLT_EN, OCP_FLT_EN);
    bl_update(b, 0u, PWMBL_INT_EN, PWMBL_INT_MASK, PWMBL_INT_MASK);
    bl_update(b, 0u, PWMBL_ANA1, ANA1_MAX_BRIGHT_MASK, 3u);
    bl_update(b, 0u, PWMBL_ANA1, ANA1_LOAD_CUR_MASK, 0u);
    bl_update(b, 0u, PWMBL_CTRL, PWMBL_CTRL_CLK_GATING, PWMBL_CTRL_CLK_GATING);
    bl_update(b, 0u, PWMBL_ANA0, 1u, 1u);

    b->enabled = RT_FALSE;
    bl_level(b, 0u);
}

static rt_err_t pwm_bl_control(struct rt_device_pwm *device, int cmd, void *args)
{
    struct sun_pwm_bl *b = rt_container_of(device, struct sun_pwm_bl, pwm);
    struct rt_pwm_configuration *cfg = (struct rt_pwm_configuration *)args;

    if (cfg->channel != 0)
        return -RT_EINVAL;

    switch (cmd)
    {
    case PWM_CMD_ENABLE:
        b->enabled = RT_TRUE;
        bl_level(b, b->level);
        break;
    case PWM_CMD_DISABLE:
        b->enabled = RT_FALSE;
        bl_update(b, 0u, PWMBL_CTRL, PWMBL_CTRL_ENABLE, 0u);
        bl_update(b, 1u, PWMBL_CTRL, PWMBL_CTRL_ENABLE, 0u);
        break;
    case PWM_CMD_SET:
        if (cfg->period == 0u || cfg->pulse > cfg->period)
            return -RT_EINVAL;
        b->level = (rt_uint32_t)((rt_uint64_t)cfg->pulse * PWMBL_MAX_LOAD_CUR / cfg->period);
        if (b->enabled)
            bl_level(b, b->level);
        break;
    case PWM_CMD_GET:
        break;
    default:
        return -RT_EINVAL;
    }
    return RT_EOK;
}

static const struct rt_pwm_ops pwm_bl_ops =
{
    pwm_bl_control,
};

static rt_err_t pwm_bl_probe(struct rt_platform_device *pdev)
{
    rt_err_t err;
    struct rt_device *dev = &pdev->parent;
    struct sun_pwm_bl *b = rt_calloc(1, sizeof(*b));
    struct rt_clk *bus, *mod;
    struct rt_reset_control *rst;

    if (!b)
        return -RT_ENOMEM;

    b->base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    bus = rt_clk_get_by_name(dev, "bus");
    mod = rt_clk_get_by_name(dev, "mod");
    rst = rt_reset_control_get_by_index(dev, 0);
    if (!b->base || rt_is_err_or_null(bus) || rt_is_err_or_null(mod) || rt_is_err_or_null(rst))
    {
        rt_free(b);
        return -RT_ERROR;
    }
    rt_reset_control_deassert(rst);
    rt_clk_prepare_enable(bus);
    rt_clk_set_rate(mod, PWMBL_MOD_RATE);
    rt_clk_prepare_enable(mod);

    bl_hw_init(b);

    b->pwm.parent.ofw_node = dev->ofw_node;
    dev->user_data = b;
    rt_dm_dev_bind_fwdata(&b->pwm.parent, RT_NULL, &b->pwm);
    rt_dm_dev_set_name_auto(&b->pwm.parent, "pwm_bl");
    err = rt_device_pwm_register(&b->pwm, rt_dm_dev_get_name(&b->pwm.parent), &pwm_bl_ops, b);
    if (err)
        rt_free(b);

    return err;
}

static const struct rt_ofw_node_id pwm_bl_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-pwm-bl" },
    { /* sentinel */ }
};

static struct rt_platform_driver pwm_bl_driver =
{
    .name = "pwm-bl-sun252i",
    .ids = pwm_bl_ofw_ids,
    .probe = pwm_bl_probe,
};
RT_PLATFORM_DRIVER_EXPORT(pwm_bl_driver);
