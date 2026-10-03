/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PWM controller of the sun252i SoC, four channels. A period below 334 ns uses the fast
 * clock bypass, others a power of two divider and an 8 bit prescaler of the 24 MHz reference.
 */
#include <rtthread.h>
#include <rtdevice.h>

#define PWM_PCGR        0x40u
#define PWM_PER         0x80u
#define PWM_PCR         0x100u
#define PWM_PPR         0x104u
#define PWM_CH_STRIDE   0x20u

#define PWM_PCCR_BASE   0x20u
#define PWM_PCCR_STRIDE 0x04u
#define PWM_PCCR_DIV_M  0x0000000fu
#define PWM_PCCR_SRC    0x00000180u

#define PWM_PCR_PRESCALE 0x000000ffu
#define PWM_PCR_POLARITY 0x00000100u

#define PWM_PCGR_GATE(ch)   (1u << (ch))
#define PWM_PCGR_BYPASS(ch) (1u << ((ch) + 16u))
#define PWM_PER_ENABLE(ch)  (1u << (ch))

#define PWM_CHANNELS    4u
#define PWM_REF_CLK     24000000ull
#define PWM_FAST_CLK    100000000ull


struct sun_pwm
{
    struct rt_device_pwm pwm;
    rt_ubase_t base;
};

static rt_uint32_t pwm_read(struct sun_pwm *p, rt_uint32_t off)
{
    return HWREG32(p->base + off);
}

static void pwm_write(struct sun_pwm *p, rt_uint32_t off, rt_uint32_t val)
{
    HWREG32(p->base + off) = val;
}

static void pwm_update(struct sun_pwm *p, rt_uint32_t off, rt_uint32_t mask, rt_uint32_t value)
{
    pwm_write(p, off, (pwm_read(p, off) & ~mask) | (value & mask));
}

static rt_uint32_t pwm_pccr_offset(rt_uint32_t channel)
{
    return PWM_PCCR_BASE + (channel / 2u) * PWM_PCCR_STRIDE;
}

static rt_err_t pwm_set_ns(struct sun_pwm *p, int channel, rt_uint32_t period,
        rt_uint32_t pulse)
{
    rt_uint32_t ch = (rt_uint32_t)channel & 0xffu;
    rt_uint64_t source_hz;
    rt_uint64_t source_cycles;
    rt_uint32_t div_m = 0u, prescale = 0u, period_count = 0u, active_count;
    rt_uint32_t pccr, pcr, ppr;

    if (ch >= PWM_CHANNELS || period == 0u || pulse > period)
        return -RT_EINVAL;

    if (pulse == 0u)
    {
        /* a zero duty cycle gates the output off */
        pwm_update(p, PWM_PCGR, PWM_PCGR_GATE(ch), 0u);
        return RT_EOK;
    }

    if (period <= 334u)
    {
        /* the fast clock bypasses the divider and emits the source clock */
        source_hz = PWM_FAST_CLK;
        pccr = pwm_read(p, pwm_pccr_offset(ch));
        pccr = (pccr & ~(PWM_PCCR_SRC | PWM_PCCR_DIV_M)) | (1u << 7u);
        pwm_write(p, pwm_pccr_offset(ch), pccr);
        pwm_update(p, PWM_PCGR, PWM_PCGR_GATE(ch) | PWM_PCGR_BYPASS(ch),
                   PWM_PCGR_GATE(ch) | PWM_PCGR_BYPASS(ch));
        pwm_update(p, PWM_PER, PWM_PER_ENABLE(ch), PWM_PER_ENABLE(ch));
        return RT_EOK;
    }

    source_hz = PWM_REF_CLK;
    source_cycles = (source_hz * period + 500000000ull) / 1000000000ull;
    if (source_cycles == 0u)
        return -RT_EINVAL;

    /* a power-of-two divider (1..256) and an 8 bit prescaler pick the period */
    for (div_m = 0u; div_m <= 8u && period_count == 0u; div_m++)
    {
        rt_uint32_t m = 1u << div_m;
        rt_uint32_t p;

        for (p = 0u; p <= 255u; p++)
        {
            rt_uint64_t count = source_cycles / m / (p + 1u);

            if (count >= 1u && count <= 65536u)
            {
                period_count = (rt_uint32_t)count;
                prescale = p;
                break;
            }
        }
    }
    if (period_count == 0u)
        return -RT_EINVAL;

    active_count = (rt_uint32_t)(((rt_uint64_t)period_count * pulse) / period);
    if (active_count == 0u)
        active_count = 1u;
    if (active_count > period_count)
        active_count = period_count;

    pccr = pwm_read(p, pwm_pccr_offset(ch));
    pccr &= ~(PWM_PCCR_SRC | PWM_PCCR_DIV_M);
    pccr |= div_m;
    pwm_write(p, pwm_pccr_offset(ch), pccr);

    pcr = pwm_read(p, PWM_PCR + ch * PWM_CH_STRIDE);
    pcr &= ~(PWM_PCR_PRESCALE | PWM_PCR_POLARITY);
    pcr |= prescale;
    pwm_write(p, PWM_PCR + ch * PWM_CH_STRIDE, pcr);

    ppr = (period_count - 1u) << 16u;
    ppr |= active_count & 0xffffu;
    pwm_write(p, PWM_PPR + ch * PWM_CH_STRIDE, ppr);

    /* program the channel before enabling its output */
    pwm_update(p, PWM_PCGR, PWM_PCGR_BYPASS(ch), 0u);
    pwm_update(p, PWM_PCGR, PWM_PCGR_GATE(ch), PWM_PCGR_GATE(ch));
    pwm_update(p, PWM_PER, PWM_PER_ENABLE(ch), PWM_PER_ENABLE(ch));

    return RT_EOK;
}

static rt_err_t pwm_control(struct rt_device_pwm *device, int cmd, void *args)
{
    struct sun_pwm *p = rt_container_of(device, struct sun_pwm, pwm);
    rt_err_t ret = RT_EOK;
    struct rt_pwm_configuration *cfg = (struct rt_pwm_configuration *)args;
    rt_uint32_t ch = cfg->channel;

    if (ch >= PWM_CHANNELS)
        return -RT_EINVAL;

    switch (cmd)
    {
    case PWM_CMD_ENABLE:
        pwm_update(p, PWM_PCGR, PWM_PCGR_GATE(ch), PWM_PCGR_GATE(ch));
        pwm_update(p, PWM_PER, PWM_PER_ENABLE(ch), PWM_PER_ENABLE(ch));
        break;
    case PWM_CMD_DISABLE:
        pwm_update(p, PWM_PCGR, PWM_PCGR_GATE(ch), 0u);
        break;
    case PWM_CMD_SET:
        ret = pwm_set_ns(p, (int)ch, cfg->period, cfg->pulse);
        break;
    case PWM_CMD_GET:
        break;
    default:
        ret = -RT_EINVAL;
        break;
    }
    return ret;
}

static const struct rt_pwm_ops pwm_ops =
{
    pwm_control,
};

static rt_err_t pwm_probe(struct rt_platform_device *pdev)
{
    rt_err_t err;
    struct rt_device *dev = &pdev->parent;
    struct sun_pwm *p = rt_calloc(1, sizeof(*p));
    struct rt_clk *clk;
    struct rt_reset_control *rst;

    if (!p)
    {
        return -RT_ENOMEM;
    }

    p->base = (rt_ubase_t)rt_dm_dev_iomap(dev, 0);
    clk = rt_clk_get_by_index(dev, 0);
    rst = rt_reset_control_get_by_index(dev, 0);
    if (!p->base || rt_is_err_or_null(clk) || rt_is_err_or_null(rst))
    {
        rt_free(p);

        return -RT_ERROR;
    }
    rt_reset_control_deassert(rst);
    rt_clk_prepare_enable(clk);

    for (rt_uint32_t ch = 0; ch < PWM_CHANNELS; ch++)
    {
        /* gate every channel off; setting the period turns the requested one on */
        pwm_update(p, PWM_PCGR, PWM_PCGR_GATE(ch), 0u);
        pwm_update(p, PWM_PER, PWM_PER_ENABLE(ch), 0u);
    }

    p->pwm.parent.ofw_node = dev->ofw_node;
    dev->user_data = p;
    rt_dm_dev_bind_fwdata(&p->pwm.parent, RT_NULL, &p->pwm);
    rt_dm_dev_set_name_auto(&p->pwm.parent, "pwm");
    err = rt_device_pwm_register(&p->pwm, rt_dm_dev_get_name(&p->pwm.parent), &pwm_ops, p);
    if (err)
    {
        rt_free(p);
    }

    return err;
}

static const struct rt_ofw_node_id pwm_ofw_ids[] =
{
    { .compatible = "allwinner,sun252i-pwm" },
    { /* sentinel */ }
};

static struct rt_platform_driver pwm_driver =
{
    .name = "pwm-sun252i",
    .ids = pwm_ofw_ids,
    .probe = pwm_probe,
};
RT_PLATFORM_DRIVER_EXPORT(pwm_driver);
